---
id: 003
slug: dirty-log-guest-no-huge-page-support
title: __pkvm_host_dirty_log_guest() assumes a 4 KiB mapping, so KVM_RUN fails with -E2BIG on any huge-page-backed guest once dirty logging is enabled
class: kernel-defect
signature: 'KVM_RUN returns -E2BIG after KVM_MEM_LOG_DIRTY_PAGES is enabled'
hazard: none
diagnosis: root-caused
disposition: open
repro: repro/probe-dirtylog-thp.c
observations:
  - target: 'klinux 6.6.103+ #4 @348c94763cc6'
    state: reproduced
    run: 2026-08-05-dirtylog-e2big
    evidence: evidence/2026-08-05-mode-matrix.txt
---

# 003 — dirty logging is unusable for a huge-page-backed guest

中文版本:[ISSUE_zh.md](ISSUE_zh.md)

On a `kvm-arm.mode=protected` host, enabling `KVM_MEM_LOG_DIRTY_PAGES` on a guest whose memory is backed by transparent huge pages makes the guest **unable to resume**: the next `KVM_RUN` returns `-E2BIG` and the vCPU never makes progress again. Transparent huge pages are the default, so this is the ordinary configuration, and dirty logging is the mechanism behind live migration and snapshotting.

The board is unaffected — no hang, no Oops. Only the guest dies.

## Symptom

```
first  KVM_RUN ret=0 errno=0 exit=6            (KVM_EXIT_MMIO — guest ran fine)
after dirty logging on
second KVM_RUN ret=-1 errno=7 (Argument list too long) exit=0
```

`errno 7` is `E2BIG`. Deterministic: 5 runs, 5 failures.

## Mechanism

**Confidence: root-caused.** A hardware A/B varying only the page size isolates the cause, and the code chain accounts for it exactly.

**The measurement.** Three modes, five runs each (`evidence/2026-08-05-mode-matrix.txt`):

| mode | guest memory | dirty logging | `KVM_RUN` failures |
| --- | --- | --- | --- |
| default | 2 MiB THP block | on | **5/5** |
| `nohuge` | forced 4 KiB via `MADV_NOHUGEPAGE` | on | **0/5** |
| `nodirty` | 2 MiB THP block | off | **0/5** |

Both conditions are necessary; neither alone reproduces.

**The chain**, every link read in full:

```
guest memory mapped by a 2 MiB block
  → dirty logging enabled → write-protect
  → guest write → stage-2 permission fault
  → mmu.c:1760   kvm_call_hyp_nvhe(__pkvm_host_dirty_log_guest, gfn)   ← gfn only, no order
  → EL2 permissions.rs:147  guest_get_valid_pte(&mut host_addr, guest_addr, 0, &mut pte)
                                                                        ↑ order hardcoded to 0
  → EL2 guest.rs:1598,1618   size = PAGE_SIZE << 0 = 4 KiB
                             if kvm_granule_size(level) != size { return -E2BIG }
                                          ↑ actual granule is 2 MiB → mismatch
  → returned verbatim to the host (permissions.rs:148-152), out through
    pkvm_relax_perms()'s logging branch (mmu.c:1760-1762), out of KVM_RUN
```

**Structural confirmation — this is one missed conversion, not a design choice.** `guest_get_valid_pte()` has four callers; three were given a real `order` by the pKVM huge-page series and one was not:

```
host.rs:1669        guest_get_valid_pte(..., order, ...)
host.rs:1714        guest_get_valid_pte(..., order, ...)
host.rs:1897        guest_get_valid_pte(..., order, ...)
permissions.rs:147  guest_get_valid_pte(..., 0,     ...)   ← dirty logging
```

The host-side HVC ABI shows the same gap:

```c
kvm_call_hyp_nvhe(__pkvm_host_wrprotect_guest,   handle, gfn, order);      /* mmu.c:1361 */
kvm_call_hyp_nvhe(__pkvm_host_relax_guest_perms,         gfn, order, prot); /* mmu.c:1730 */
kvm_call_hyp_nvhe(__pkvm_host_dirty_log_guest,           gfn);             /* mmu.c:1760 */
```

And the series itself, in this tree, covers five paths and not this one:

```
275629ed658f  THP support for pKVM guests
bb35d8934803  Huge page support for pKVM guest relax perm
5b77817a1876  Huge page support for pKVM guest wrprotect
972a2211ac11  Huge page support for pKVM guest unshare
ac23d6fd5aba  Huge page support for pKVM guest memory reclaim
b852c9e9fcea  Huge page support for pkvm_pinned_page
              (no "Huge page support for pKVM guest dirty log")
```

So write-protecting a block works, and relaxing permissions on a block works — but the moment a write-protected *block* takes a write fault and control enters the dirty-logging branch, it reaches the one path that was never converted.

**Not yet observed directly:** the `-E2BIG` was not watched leaving `guest_get_valid_pte()` at EL2. The A/B varies exactly the granule and nothing else, and the granule check is the only `-E2BIG` on the path, but an EL2-coverage capture of the two variants would close the last step by showing the line executed only in the failing one.

## Trigger

1. host booted `kvm-arm.mode=protected`
2. an **ordinary** VM (`KVM_CREATE_VM` type 0) — protected VMs take a different path
3. guest memory backed by a huge page (the default for anonymous memory)
4. a vCPU that has **run**, so a hyp VM exists and pages are mapped through EL2
5. `KVM_MEM_LOG_DIRTY_PAGES` added to an existing memslot
6. the guest writes to a write-protected page

## Reproduction

`repro/probe-dirtylog-thp.c`, three modes in one binary:

```
aarch64-linux-gnu-gcc -O2 -static -o probe-dirtylog-thp repro/probe-dirtylog-thp.c
./probe-dirtylog-thp            # THP + dirty logging  -> KVM_RUN -1/E2BIG
./probe-dirtylog-thp nohuge     # 4 KiB + dirty logging -> succeeds
./probe-dirtylog-thp nodirty    # THP, no dirty logging -> succeeds
```

It also samples `kvm->stat.protected_hyp_mem` from `/sys/kernel/debug/kvm/<pid>-<vmfd>/` at each step. Harmless: bounded by `alarm()`, no hang, safe on a board you cannot power-cycle.

## Blast radius

**Live migration and snapshotting do not work on this platform in the default configuration.** Any VMM that enables dirty logging on a guest with THP-backed memory — which is what a VMM gets unless it asks otherwise — will see the guest stop dead. The failure is at least loud: `KVM_RUN` returns an error rather than corrupting anything.

Workaround available today, measured: `madvise(MADV_NOHUGEPAGE)` on the guest memory before the guest faults it in, at the cost of the TLB benefit of huge pages.

## Co-observed, and NOT the same defect

Every run that enables dirty logging also leaves `kvm->stat.protected_hyp_mem` negative at VM teardown, which `arm.c:263` reports as:

```
kvm [35488]: 18446744073709543424B of donations to the nVHE hyp are missing
```

`18446744073709543424` is `0xFFFFFFFFFFFFE000` = **−8192**, i.e. −2 pages: the check reads a signed `atomic64_t` and prints it with `%llu`, and its wording assumes a leak when the imbalance is the other way.

The mode matrix separates it from this issue: it appears in **both** dirty-logging modes, including the one where `KVM_RUN` succeeds, and never in `nodirty`. So it is keyed to enabling dirty logging, not to huge pages and not to the `-E2BIG` failure. It needs its own id; the leading candidate is that pages topped up through `handle_hyp_req_mem()` (`handle_exit.c:369`) never reach `protected_hyp_mem`, while `kvm_arch_vcpu_destroy()` (`arm.c:512`) subtracts the whole of `stage2_mc.nr_pages` — unverified.

That thread was named as the most promising OOM candidate on 2026-07-30 and recorded as *"Unverified — no evidence gathered yet"* (`notes/pkvm/evidence/finding-oom-leak-and-mmu-topup-oops-2026-07-30/ROOT-CAUSE-ANALYSIS.md:221`). It now has a deterministic reproducer. Note that document also states `handle_hyp_req_mem()` "accounts them into `kvm->stat.protected_hyp_mem`" — by the code in this tree it does not, which may be why the thread stopped there.

## Why this surfaced only now — and the part about issue 002

It was **not** exposed by fixing issue 002. Every observation above is on `#4`, which does not contain that fix; the fix is built as `#5` and is not deployed. Nor was it exposed by the issue 001 fix: the 001 reproducer exercises the code that fix changed, heavily, and produces none of these signatures.

What exposed it was the *requirement to verify* 002. Review established that 002's reproducer could never validate 002's fix, because it creates no vCPU: with `pkvm.handle` still 0, EL2 maps the handle to an out-of-range index and returns without flushing, so after the fix the warning would vanish because the call became a no-op. Verifying 002 therefore demanded a program that **runs a vCPU first** and only then enables dirty logging and resumes.

Nothing in this project had ever produced that sequence. 001's reproducer runs a vCPU but never touches dirty logging. 002's touches dirty logging but never runs a vCPU. The fuzzing campaigns enable both `ioctl$KVM_SET_USER_MEMORY_REGION` and `ioctl$KVM_RUN`, yet the 2026-07-31 and 2026-08-05 console logs contain zero instances of this signature — those runs never happened to compose the two against one VM in the required order. The first execution of that composition failed immediately and has failed every time since.

There is, separately, a real sense in which **fixing 002 will unmask 003 on hardware where it might otherwise hide**. Reaching the broken path requires the guest's write to actually fault. Before the 002 fix the write-protect is applied but the TLB invalidation that should make it visible is refused, so whether the guest faults depends on the CPU having evicted its stale writable entry. On N90 it faults anyway — that is exactly why `-E2BIG` is observable here on `#4` — but that is a property of this hardware and timing, not a guarantee. With 002 fixed the invalidation actually happens, and the fault, and therefore this failure, becomes reliable.

The order matters for anyone reading the tracker later: **002 does not cause 003, and 003 is not a regression from either of our patches.** 003 is pre-existing klinux behaviour that a new input shape reached for the first time, and 002's fix makes reaching it deterministic rather than incidental.

## Residual hazard

This blocks verification of issue 002 by its intended route. The plan was to prove the range TLB flush works by enabling dirty logging on a running guest and checking that post-write-protect writes land in the dirty bitmap — which cannot complete while the guest cannot resume. Either 003 is fixed first, or 002 is verified another way: an EL2-coverage capture showing `handle___pkvm_tlb_flush_vmid` executing, or the same dirty-bitmap check run under `MADV_NOHUGEPAGE`, which the matrix shows does resume.
