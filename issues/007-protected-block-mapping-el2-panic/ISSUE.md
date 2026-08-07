---
id: 007
slug: protected-block-mapping-el2-panic
title: guest_get_page_state() tests the raw PTE instead of the extracted prot, so every 2 MiB block mapping in a protected guest reports an impossible page state and tearing that guest down hits bug_on!() in EL2 — where the Rust panic handler is loop {}, so the CPU never returns and the board dies
class: kernel-defect
signature: 'board wedges: CPU stops answering IPI/RCU/NMI, cascading soft lockups'
hazard: wedges-target
diagnosis: root-caused
disposition: open
repro: repro/probe-thp-kcov.c
observations:
  - target: 'klinux 6.6.103+ #25 @a89de9c463bc'
    state: reproduced
    run: 2026-08-07-007-thp-block-mapping
    evidence: evidence/ring-dump-cpu7-hang.txt
---

# 007 — a 2 MiB guest mapping makes EL2 panic, and the panic handler is `loop {}`

Found while adding `syz_kvm_run_protected_guest$arm64`, the first composite in this fork that runs a guest inside a **protected** VM. Reproduced by hand the same evening, root-caused to a single-token porting error, and localised by reading the EL2 program counters out of a CPU that was already dead.

**Two independent defects.** One makes EL2 hit a `bug_on!`; the other turns any EL2 `bug_on!` into a silent whole-board hang. The second is the more serious of the two, and it is why this cost five power cycles to diagnose.

## Symptom

A protected VM whose memslot is left THP-eligible never finishes teardown. `close(vm)` does not return; the process is killed at the executor's 46 s timeout. One CPU then stops reporting RCU quiescent states and stops answering NMI:

```
rcu: INFO: rcu_sched detected stalls on CPUs/tasks:
rcu:     6-...0: (1 GPs behind) idle=db74/1/0x4000000000000000 softirq=22666/22676 fqs=6527
rcu:     (detected by 2, t=15002 jiffies, g=27013, q=7446 ncpus=8)
Sending NMI from CPU 2 to CPUs 6:                 <- no backtrace ever follows
```

Everything after that is downstream. `cleanup_net` blocks forever in `synchronize_rcu()` under `hsr_del_port` **while holding `rtnl_lock`**, and every task that touches networking piles up behind it; `sshd` soft-locks in `kick_all_cpus_sync()` because seccomp's BPF JIT calls `__text_poke()`. The board answers ping and refuses ssh. Only a power cycle recovers it.

Full cascade: [evidence/console-boot2-thp-isolated.txt](evidence/console-boot2-thp-isolated.txt).

## Trigger

Single-variable isolation, one boot, one sandbox, three programs:

| program | wall time | coverage | board |
| --- | --- | --- | --- |
| `syz_kvm_dirty_log_cycle(hp_mode=0)` — **ordinary** VM | ~1 s | 84318 | clean |
| `syz_kvm_run_protected_guest(hp_mode=0)` — protected, `MADV_NOHUGEPAGE` | ~1 s | 69566 | clean |
| `syz_kvm_run_protected_guest(hp_mode=1)` — protected, THP left eligible | **46 s** | **0** | **wedged** |

The only difference between rows 2 and 3 is one `madvise(MADV_NOHUGEPAGE)` call. A protected VM is required (an ordinary VM's pinned pages are drained by `pkvm_unmap_range()` before teardown ever reaches the reclaim loop — see the note in `sys/linux/dev_kvm_arm64.txt`), and a block mapping is required.

No pvmfw is needed, which is why no pre-existing composite reaches this. Of the protected-VM composites only two ever run a guest — `syz_kvm_run_fw_fault` and `syz_kvm_run_fw_fault_gen` — and both return at their `firmware_size == 0` INFO gate on a board with no pvmfw reservation, before creating a vCPU; the rest are configuration-only paths that never run a guest. With `pvmfw_load_addr` left invalid, EL2's `pkvm_vcpu_init_psci()` takes the reset PC straight from the host vCPU registers, so a plain `KVM_SET_ONE_REG(pc)` boots the guest.

## Root cause

### Defect A — `pte` where `prot` was meant

`arch/arm64/kvm/hyp/xhypervisor/src/mem_protect/guest.rs`, `guest_get_page_state()`:

```rust
let prot = kvm_pgtable_stage2_pte_prot(pte);
// 前面已经判断了页表项有效，这里应该就不用再次判断了
// pte 只是 u64 类型，其值也不会改变
if (pte & KVM_PGTABLE_PROT_RWX) != KVM_PGTABLE_PROT_RWX {
    state = PKVM_PAGE_RESTRICTED_PROT;
}
```

against ACK `android16-6.12` (`arch/arm64/kvm/hyp/nvhe/mem_protect.c`):

```c
prot = kvm_pgtable_stage2_pte_prot(pte);
if (kvm_pte_valid(pte) && ((prot & KVM_PGTABLE_PROT_RWX) != KVM_PGTABLE_PROT_RWX))
	state = PKVM_PAGE_RESTRICTED_PROT;
```

The comment shows the porter removing the redundant `kvm_pte_valid(pte)` test. In doing so the second half of the condition lost `prot` as well: it now masks the raw 64-bit page-table entry.

These are not two offsets into the same word — they are two namespaces. `KVM_PGTABLE_PROT_R/W/X` are bits 0/1/2 of `enum kvm_pgtable_prot`, a value that does not exist in the PTE at all; the hardware keeps permissions in S2AP (bits 6–7) and XN (bits 53–54), and `kvm_pgtable_stage2_pte_prot()` *translates* those fields into the enum. Masking `pte` with `0b111` therefore lands on Valid/Type/MemAttr, which have nothing to do with permission. That is why the typo does not degrade into "permissions read slightly wrong" but into "the descriptor type decides the state flag".

`KVM_PGTABLE_PROT_X = BIT(0)`, `W = BIT(1)`, `R = BIT(2)`, so `KVM_PGTABLE_PROT_RWX == 0b111`. The low three bits of a valid stage-2 descriptor are:

| | bit 0 `KVM_PTE_VALID` | bit 1 `KVM_PTE_TYPE` | bit 2 (low bit of MemAttr; 1 for normal memory) | `pte & 7` |
| --- | --- | --- | --- | --- |
| 4 KiB page, last level | 1 | **1** = `KVM_PTE_TYPE_PAGE` | 1 | `0b111` = 7 |
| 2 MiB block, level 2 | 1 | **0** = `KVM_PTE_TYPE_BLOCK` | 1 | `0b101` = 5 |

For a **block** descriptor `(pte & 7) != 7` is therefore *always* true, and `PKVM_PAGE_RESTRICTED_PROT` is set unconditionally. Guest RAM donated to a protected VM is `PKVM_PAGE_OWNED`, whose value is 0, so the state returned is `PKVM_PAGE_RESTRICTED_PROT` on its own — which matches none of the four arms in `__pkvm_host_reclaim_page()`:

```rust
match page_state as u32 {
    PKVM_PAGE_OWNED                                            => { ... }
    PKVM_PAGE_SHARED_BORROWED                                  => { ... }
    s if s == (PKVM_PAGE_SHARED_BORROWED | PKVM_PAGE_RESTRICTED_PROT) => { ... }
    PKVM_PAGE_SHARED_OWNED                                     => { ... }
    _ => { bug_on!(true); }          // <- the only bug_on! in this function
}
```

For a **page** descriptor bits 0, 1 and 2 are all set, so `pte & 7` is 7, the condition is false, the state stays `OWNED`, and the first arm runs. That is exactly the observed `hp_mode=0` works / `hp_mode=1` wedges asymmetry, with no further assumption required.

### Defect B — the EL2 panic handler is an infinite loop

`arch/arm64/kvm/hyp/xhypervisor/src/lib.rs:75`:

```rust
#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    loop {}
}
```

`bug_on!(cond)` expands to `panic!("BUG_ON condition failed")` (`bindings/mod.rs:300`), which lands here. The CPU spins at EL2 with interrupts masked: it never returns to the host, the HVC never completes, it executes no further instrumented code, and it answers neither IPI nor RCU nor NMI. Every host operation that needs all CPUs — `synchronize_rcu()`, `rcu_barrier()`, `kick_all_cpus_sync()` — then blocks forever.

The C nvhe hypervisor's `BUG_ON` goes through `hyp_panic()` and reports to the host. **In the Rust hypervisor every `bug_on!` degrades from "a reportable hyp panic" to "the board is gone, with no diagnostic whatsoever".** Defect B is independent of A and will do the same for any other EL2 bug.

## Confirmed on hardware

Localisation, in order, each step narrowing the previous one:

| question | instrument | answer |
| --- | --- | --- |
| donation or teardown? | phase markers written to `/dev/kmsg` (ssh dies; stderr is lost) | teardown: `KVM_RUN` returns `KVM_EXIT_MMIO`, `close(vm)` never returns |
| host loop or one hypercall? | `function_graph` on `pkvm_destroy_hyp_vm`, one pid, streamed to `/dev/kmsg` | the **first** `__reclaim_dying_guest_page_call` never returns; the control arm returns from the same call in 12.5 µs |
| spinning or stopped? | `kvm/pkvm_cov/ring_dump` read from a healthy CPU | **stopped** |
| where exactly? | `addr2line` on the last PC | `bindings/mod.rs:300`, inlined from `__pkvm_host_reclaim_page` |

The third row is the one that redirected the whole investigation. The stuck CPU's EL2 coverage ring reads the same twenty seconds apart:

```
cpu7 flags=0x1 count=57 hits=57 dropped=0 nested=0 capacity=16381     <- t=69 s
cpu7 flags=0x1 count=57 hits=57 dropped=0 nested=0 capacity=16381     <- t=89 s
```

`flags=0x1` means `pkvm_cov_begin()` armed the window and `pkvm_cov_end()` never ran — the HVC that did not come back. A frozen `count` rules out an instrumented loop, which would have driven `hits` to the 16381 cap and set `OVERFLOW`. The last three PCs:

```
54  __kvm_nvhe___hyp_put_page+0x134
55  __kvm_nvhe___pkvm_reclaim_dying_guest_page+0x55c/0x560
56  __kvm_nvhe__RNvCs..._rustc17rust_begin_unwind+0x8/0xc
```

```
$ aarch64-linux-gnu-addr2line -e vmlinux -f -i -C 0xffff80008145bfd4
xhypervisor::mem_protect::host::__pkvm_host_reclaim_page
  arch/arm64/kvm/hyp/xhypervisor/src/bindings/mod.rs:300
__pkvm_reclaim_dying_guest_page
  arch/arm64/kvm/hyp/xhypervisor/src/pkvm.rs:1298
```

Artifacts: [ring dump](evidence/ring-dump-cpu7-hang.txt) · [hang trace](evidence/ftrace-thp1-hang.txt) · [control trace](evidence/ftrace-control-thp0.txt) · [console cascade](evidence/console-boot2-thp-isolated.txt).

`ring_dump` did not exist before this issue; it is a read-only debugfs export of the raw, undrained ring, added because a wedged EL2 has no other channel — it cannot print, and the host cannot interrupt it. Patch: [repro/ring-dump.patch](repro/ring-dump.patch).

### Ruled out

- **The EL2 KCOV bridge.** Suspected because the last traced host call before the hang is `pkvm_cov_begin()`, and because order 9 produces orders of magnitude more SanCov callbacks than order 0, so a ring overflow was a plausible order-only trigger. Stopped `pkvm-cov-arm.service`, wrote `0` to `enable`, confirmed `armed_cpus 0`, re-ran: **the board wedged at the identical marker.**
- **A host-side loop.** Every branch of `pkvm_call_hyp_nvhe_ppage()`'s `while` terminates: the success path returns once `pins` reaches 0, `-E2BIG` can demote `order` 9 → 0 only once, and `-ENOENT` decrements `size` every iteration. The trace independently shows it never completes its first call.
- **An EL2 infinite loop.** Frozen `count`, above.

## Blast radius

Any process that can open `/dev/kvm` can wedge the machine: create a VM with `KVM_VM_TYPE_ARM_PROTECTED`, register a THP-eligible memslot, run one instruction, exit. No privilege beyond `/dev/kvm` and no special hardware — in particular no pvmfw — is required. `repro/probe-thp-kcov.c` is ~180 lines and takes under a second to reach the hang.

Because the failure is a hypervisor-level infinite loop rather than a host fault, none of the usual host mitigations apply: the task cannot be killed, the CPU cannot be offlined, and `sysrq` cannot reach it.

## Fix status

`open`. Nothing has been changed in the hypervisor — this entry records the diagnosis only.

Both defects need fixing, and **B should go first**: until the panic handler reports instead of hanging, every other EL2 defect is equally undiagnosable, and this one took five power cycles for exactly that reason.

## Residual hazard

**Handled.** `syz_kvm_run_protected_guest$arm64`'s `hp_mode` has been narrowed from `int8[0:1]` to `const[0]` (`sys/linux/dev_kvm_arm64.txt`, with the reasoning and the condition for widening it recorded at that line), so a campaign cannot reach the THP path; the THP case stays here as `repro/probe-thp-kcov.c`.

The judgement behind that: this bug produces **no crash report**, only an unreachable board and a power cycle per hit. That is negative value for a fuzzer, so keeping it reachable costs coverage and buys nothing until the hypervisor is fixed. Widen it back to `int8[0:1]` once 007 is `fix-verified`.

## Adjacent defects found in the same function, not causal to this hang

Recorded so they are not lost; each needs its own verification and none of them is why the board hangs.

| | klinux | ACK `android16-6.12` |
| --- | --- | --- |
| `hyp_poison_page()` | `hyp_poison_page(phys)` | `hyp_poison_page(phys, page_size)` |
| `drain_hyp_pool()` (Rust only) | pushes order `0`, donates `1` page | reads `page->order`, clears it, donates `1 << order` |
| overflow guard | absent | `check_shl_overflow(PAGE_SIZE, order, &page_size)` |
| protected-VM guard | absent | `if (!pkvm_hyp_vm_is_protected(vm)) return -EPERM;` |
| `SHARED_OWNED` check failure | `warn_on!` and continue | `ret = -EBUSY; goto unlock;` |
| stage-2 unmap ordering | before the page-state switch | after it |

The first is a **confidentiality** defect in its own right: at order 9 only the first 4 KiB of a 2 MiB block is poisoned, so 2044 KiB of protected-guest memory is returned to the host unscrubbed.

It is **not** inherited from upstream: ACK declares `void hyp_poison_page(phys_addr_t phys, size_t page_size)`, while klinux's `hyp/include/nvhe/mm.h:22` declares `void hyp_poison_page(phys_addr_t phys)`. The size parameter was dropped locally, and **the C and Rust implementations diverge together** — so it is neither an upstream defect nor a Rust-port artifact.

Working record, including the two hypotheses that were killed along the way: [notes/pkvm/evidence/2026-08-07-protected-thp-lockup/FINDING.md](../../notes/pkvm/evidence/2026-08-07-protected-thp-lockup/FINDING.md).
