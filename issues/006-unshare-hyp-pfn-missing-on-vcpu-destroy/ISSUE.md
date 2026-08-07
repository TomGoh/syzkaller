---
id: 006
slug: unshare-hyp-pfn-missing-on-vcpu-destroy
title: init_pkvm_hyp_vcpu() leaks the EL2 pin on the host vCPU struct when KVM_MP_STATE_SUSPENDED is set before the first KVM_RUN, permanently poisoning the pages so teardown WARNs and later KVM_CREATE_VCPU fails
class: kernel-defect
signature: 'WARNING in kvm_unshare_hyp'
hazard: none
diagnosis: root-caused
disposition: fix-verified
repro: repro/probe-mpstate-suspended-pin-leak.c
observations:
  - target: 'klinux 6.6.103+ #16 @cba248683e5c'
    state: reproduced
    run: 2026-08-06-n90-full-surface
    evidence: evidence/2026-08-07-what-was-ruled-out.txt
  - target: 'klinux 6.6.103+ #18 @8bdbd1873442'
    state: not-observed
    run: 2026-08-07-006-fix-deploy-verify
    evidence: evidence/2026-08-07-fix-verified-on-18.txt
---

# 006 — a suspended vCPU leaks its EL2 pin, and the leak is permanent

Found by the 2026-08-06 full-surface campaign — the first finding here that is not on the dirty-logging path. Two campaign occurrences (05:22:32, 07:30:12), neither of which syzkaller could reduce to a reproducer. Root-caused and reproduced by hand the same morning.

**It is an inherited upstream defect, already fixed in ACK, and it is reachable by any process that can open `/dev/kvm`.**

Full working record, including three hypotheses that were killed along the way: [evidence/2026-08-07-what-was-ruled-out.txt](evidence/2026-08-07-what-was-ruled-out.txt).

## Symptom

```
WARNING: CPU: 0 PID: 474382 at arch/arm64/kvm/mmu.c:728 kvm_unshare_hyp+0x12c/0x140
Comm: syz.0.6493   6.6.103+ #16   Source Version: cba248683e5c

 kvm_unshare_hyp+0x12c/0x140        arch/arm64/kvm/mmu.c:728
 kvm_arm_vcpu_destroy+0x34/0xf8     arch/arm64/kvm/reset.c:160   <- the vCPU struct
 kvm_arch_vcpu_destroy+0x5c/0xb0    arch/arm64/kvm/arm.c:536
 kvm_destroy_vcpus -> kvm_arch_destroy_vm -> kvm_destroy_vm -> kvm_vm_release -> __fput
```

One WARN per page of the `struct kvm_vcpu` (three on this build). Non-fatal — the board stayed up and the campaign kept running.

## Root cause

`init_pkvm_hyp_vcpu()` pins the host vCPU struct into EL2, then validates, then records the pointer it would need in order to *un*pin:

```rust
// xhypervisor/src/pkvm.rs — identical shape in hyp/nvhe/pkvm.c:574
hyp_pin_shared_mem(host_vcpu, host_vcpu + 1)      // 1816  pin taken
self.vcpu.arch.hyp_reqs = ...; pin hyp_reqs       // 1824  field set
if host_vcpu.vcpu_idx != vcpu_idx -> goto_done    // 1836  error path
if mp_state != RUNNABLE && != STOPPED -> goto_done// 1843  error path
self.host_vcpu = host_vcpu;                       // 1849  RECORDED HERE — too late
```

`goto_done` calls `unpin_host_vcpu()`, which unpins through the **field**:

```rust
let host_vcpu = self.host_vcpu;   // still NULL on the 1836/1843 paths
if !host_vcpu.is_null() { hyp_unpin_shared_mem(...) }   // skipped
let hyp_reqs = self.vcpu.arch.hyp_reqs;                 // was set, so this one runs
```

The asymmetry is the entire bug: `hyp_reqs` is released because its field was assigned *before* the checks; the host vCPU pin is not, because its field is assigned *after*.

## The trigger is a legal host operation

`KVM_MP_STATE_SUSPENDED` is **10**, and `kvm_arch_vcpu_ioctl_set_mpstate()` accepts it:

```c
case KVM_MP_STATE_SUSPENDED:  kvm_arm_vcpu_suspend(vcpu);
    -> WRITE_ONCE(vcpu->arch.mp_state.mp_state, KVM_MP_STATE_SUSPENDED);
```

EL2 accepts only `RUNNABLE(0)` and `STOPPED(5)`. So suspending a vCPU before its first `KVM_RUN` — an ordinary thing for a VMM to do — takes the leaking path. No privilege beyond `/dev/kvm` is required.

## Confirmed on hardware

`repro/probe-mpstate-suspended-pin-leak.c`, N90, kernel `#16 @cba248683e5c`. Measured from the continuous console capture, **not** dmesg (see the caveat below).

| arm | `KVM_RUN` | WARNs at teardown |
| --- | --- | --- |
| baseline, no probe | — | 0 |
| control, `mp_state = RUNNABLE` | `-EINTR` (our 3 s alarm) | **0** |
| test, `mp_state = SUSPENDED` | **`-EINVAL`** | **3**, `Comm: probe-mpstate` |

Reproduced across several runs with distinct PIDs. The `-EINVAL` from `KVM_RUN` is EL2's own return propagating up through `__pkvm_create_hyp_vcpu` → `pkvm_create_hyp_vm` → `kvm_arch_vcpu_run_pid_change`.

## The leak is permanent, and that is the worse half

The refcount is leaked on the **physical pages**, which then go back to the slab. Repeat runs eventually produce:

```
FATAL: KVM_CREATE_VCPU: Invalid argument
```

A later vCPU landing on a poisoned page cannot be shared to hyp at all, so `kvm_share_hyp()` fails inside `kvm_arch_vcpu_create()` and **`KVM_CREATE_VCPU` itself starts failing**. This is unprivileged-reachable resource exhaustion: each triggering VM poisons another few pages of the vCPU slab, and nothing ever recovers them short of reboot.

It also retires two open questions:

- **Why individual runs are intermittent** — a run WARNs only if the slab hands it fresh pages. If it gets already-poisoned ones, `KVM_CREATE_VCPU` fails up front and there is no teardown to WARN about.
- **Why the overnight rate climbed** — 0 occurrences in the first 13.5 h then 2 in the last 3.5 h is poisoned pages accumulating. The run record flagged this shape as an untested guess; it now has a mechanism.

## Upstream

Fixed in ACK, `Bug: 357781595`:

| commit | relevance |
| --- | --- |
| `2b4d43af6` ANDROID: KVM: arm64: Fix cleanup on partially-initialised pKVM vCPU init failure | **exactly this bug** |
| `fe4e0e499` BACKPORT: UPSTREAM: KVM: arm64: Fix pin leak and publication ordering in `__pkvm_init_vcpu()` | adjacent pin leak on the `unlock` path; cherry-pick of `73b9c1e5da84`, `Fixes: 49af6ddb8e5c`, `Cc: stable` |

`2b4d43af6`'s commit message names our path verbatim — *"leak the host_vcpu pin via the mp_state-failure path, which jumps to 'done:' before hyp_vcpu->host_vcpu is recorded"* — and fixes it by hoisting the assignment:

```c
/* Set before mp_state check so 'done:' cleanup can unpin host_vcpu. */
hyp_vcpu->host_vcpu = host_vcpu;
hyp_vcpu->vcpu.kvm = &hyp_vm->kvm;
```

`2b4d43af6` carries three further leaks on adjacent paths (hyp-pool SVE allocation on `pkvm_vcpu_init_psci()` failure; unpinning never-pinned SVE pages; `pvmfw_entry_vcpu` left dangling). **None of those is confirmed here** — N90 has no SVE, so the SVE ones are unreachable on this board — but they live in the same function and should be migrated together rather than cherry-picking only the hoist.

## Not a Rust-port artifact

The Rust EL2 is a faithful port of the pre-fix C, and `arch/arm64/kvm/hyp/nvhe/pkvm.c` in this tree carries the identical bug. That also retires the concern that our uncommitted `CONFIG_PKVM_EL2_COV` instrumentation might be responsible: the trigger is a 60-line C program that never touches the coverage ring.

## Two method notes worth keeping

**dmesg is not usable on this board during a campaign.** `vm/isolated/isolated.go:340` runs `dmesg >> dmesg-history.log; dmesg -C; dmesg -w` per instance session, so the buffer is actively cleared — observed holding a 42-second window. An earlier note in this issue attributed that to ring-buffer wrap and computed a churn rate; that was wrong, and a larger `log_buf_len` would not have helped. Use the continuous capture.

**Running this probe pollutes the campaign's crash counter,** because syzkaller's console reader sees our WARNs too. The campaign's "3rd occurrence" at 09:20 was our probe, not a fuzzer find.

## Fixed and verified

`8bdbd1873442` records `host_vcpu` as soon as its pin succeeds, so the existing `done:` cleanup covers it, and assigns `hyp_reqs` only after its own pin succeeds so that "field non-NULL" implies "page pinned" for both. Applied to the Rust EL2 and to the C, which carries the identical defect for configurations that build it.

Verified on N90, kernel `6.6.103+ #18`, after a reboot — mandatory, because the pages poisoned by the pre-fix kernel keep their leaked refcount for the life of the boot:

| | pre-fix `#16` | post-fix `#18` |
| --- | --- | --- |
| single run, `SUSPENDED` | 3 WARNs | **0** |
| 5 consecutive runs | `FATAL: KVM_CREATE_VCPU` by run 3 | **0 FATAL, 0 WARNs** |
| control, `RUNNABLE` | 0 WARNs | 0 WARNs |

`KVM_RUN` still answers `-EINVAL` for a SUSPENDED vCPU, unchanged and correct: EL2 accepts only `RUNNABLE` and `STOPPED`, and this fix concerns the cleanup on that rejection. Check dmesg, not the exit code.

Run record: [2026-08-07-006-fix-deploy-verify](../runs/2026-08-07-006-fix-deploy-verify.md).

## Still open

The three adjacent leaks `2b4d43af6` also carries — hyp-pool SVE allocation on `pkvm_vcpu_init_psci()` failure, unpinning never-pinned SVE pages, and `pvmfw_entry_vcpu` left dangling — are **not fixed here and not tested**. Two are unreachable on N90, which has no SVE, so this board cannot verify them either way. The C fix is compile-checked only; `CONFIG_X1_RMS=y` means this board runs the Rust.
