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

> **Line numbers in this document are on `klinux @348c94763cc6` (`#4`), the build every observation was made on.** Read them with `git show 348c94763cc6:<path>`. An earlier revision cited the working tree instead, which carries the issue 002 fix and is +5 lines through `mmu.c` — every `mmu.c` citation was wrong by exactly that.

On a `kvm-arm.mode=protected` host, enabling `KVM_MEM_LOG_DIRTY_PAGES` on a guest whose memory is backed by transparent huge pages makes the guest **unable to resume**: the next `KVM_RUN` returns `-E2BIG` and the vCPU never makes progress again. Transparent huge pages are the default, so this is the ordinary configuration, and dirty logging is the mechanism behind live migration and snapshotting.

The board is unaffected — no hang, no Oops. Only the guest dies.

## Symptom

```
first  KVM_RUN ret=0 errno=0 exit=6            (KVM_EXIT_MMIO — guest ran fine)
after dirty logging on
second KVM_RUN ret=-1 errno=7 (Argument list too long) exit=0
```

`errno 7` is `E2BIG`. **Frequent but not deterministic on `#4`: 22 of 25 runs (88 %)** in an independent verification; an earlier 5-run sample here happened to be 5/5, which is how this document originally came to claim determinism.

The shortfall is not reproducer flake, and it is predicted by this issue's own mechanism — see [Why this surfaced only now](#why-this-surfaced-only-now--and-the-part-about-issue-002). Reaching the broken path requires the guest's write to actually fault, and on `#4` the TLB invalidation that would make the write-protect visible is refused by issue 002. Whether the guest faults therefore depends on the CPU having evicted its stale writable entry. **Once 002 is fixed this should go to 100 %** — which is a sharper reason to fix 002 first than "it is the smaller patch".

## Mechanism

**Confidence: root-caused.** A hardware A/B varying only the page size isolates the cause, and the code chain accounts for it exactly.

**The measurement.** Three modes, five runs each (`evidence/2026-08-05-mode-matrix.txt`):

| mode | guest memory | dirty logging | `KVM_RUN` failures |
| --- | --- | --- | --- |
| default | 2 MiB THP block | on | **5/5**, and 22/25 over a larger sample |
| `nohuge` | forced 4 KiB via `MADV_NOHUGEPAGE` | on | **0/5** |
| `nodirty` | 2 MiB THP block | off | **0/5** |

Both conditions are necessary; neither alone reproduces. The `0/5` cells are what the matrix turns on, and they are hard zeros — the intermittency is only in the failing cell, for the reason given under Symptom.

**The chain**, every link read in full:

```
guest memory mapped by a 2 MiB block
  → dirty logging enabled → write-protect
  → guest write → stage-2 permission fault
  → mmu.c:1755   kvm_call_hyp_nvhe(__pkvm_host_dirty_log_guest, gfn)   ← gfn only, no order,
                                                                          and no retry wrapper
  → EL2 permissions.rs:147  guest_get_valid_pte(&mut host_addr, guest_addr, 0, &mut pte)
                                                                        ↑ order hardcoded to 0
  → EL2 guest.rs:1598,1618   size = PAGE_SIZE << 0 = 4 KiB
                             if kvm_granule_size(level) != size { return -E2BIG }
                                          ↑ actual granule is 2 MiB → mismatch
  → returned verbatim to the host (permissions.rs:148-152), out through
    pkvm_relax_perms()'s logging branch (mmu.c:1755-1757), out of KVM_RUN
```

**`-E2BIG` is a designed signal, and this is the one caller with nothing to catch it.** `pkvm_call_hyp_nvhe_ppage()` (`pkvm.c:240-268`) exists to absorb exactly this error:

```c
	int err = call_hyp_nvhe(pfn, gfn, order, args);
	switch (err) {
	/* The stage-2 huge page has been broken down */
	case -E2BIG:                        /* pkvm.c:258 */
		if (order)
			order = 0;          /* retry at 4 KiB */
		else
			return -EINVAL;     /* never lets -E2BIG escape */
		break;
```

So `-E2BIG` means "wrong order, retry smaller", and the wrapper guarantees it never reaches a caller. Three call sites use it — `mmu.c:325` (unmap), `mmu.c:1366` (write-protect), `mmu.c:1762` (relax perms). The dirty-logging call does not:

```c
	ret = kvm_call_hyp_nvhe(__pkvm_host_dirty_log_guest, gfn);   /* mmu.c:1755 — bare */
	if (ret)
		return ret;
```

**Both halves of the defect meet in the same function.** `pkvm_relax_perms()` has two branches: `logging_active` issues the bare call at `:1755`, and the `else` path goes through the wrapper at `:1762`. The dirty-logging call is simultaneously the only one whose EL2 side can never match a block (order hardcoded to 0) and the only one with no retry wrapper to catch the consequence.

**Structural confirmation on the EL2 side.** `guest_get_valid_pte()` has **three live callers** — two carry a real `order`, one passes `0`:

```
host.rs:1714        guest_get_valid_pte(..., order, ...)
host.rs:1897        guest_get_valid_pte(..., order, ...)
permissions.rs:147  guest_get_valid_pte(..., 0,     ...)   ← dirty logging
```

An earlier revision counted four, listing `host.rs:1669`. That line sits inside a commented-out duplicate of the function — `/*` at `host.rs:1635`, `*/` at `host.rs:1683`. **`mem_protect/*.rs` carries several such blocks** (`guest.rs:1521` is another copy of `guest_get_valid_pte` itself), so a grep hit in these files must be checked against comment boundaries before it is cited.

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

The mode matrix separates it from this issue: it appears in **both** dirty-logging modes, including the one where `KVM_RUN` succeeds, and never in `nodirty`. So it is keyed to enabling dirty logging, not to huge pages and not to the `-E2BIG` failure. It is filed separately as [issue 004](../004-hyp-donation-accounting-imbalance/ISSUE.md), which works the mechanism out: an unaccounted topup at `mmu.c:1749` — six lines above the hypercall this issue makes fail, in the same branch — against a teardown that subtracts the whole of `stage2_mc.nr_pages` (`arm.c:511`).

That thread was named as the most promising OOM candidate on 2026-07-30 and recorded as *"Unverified — no evidence gathered yet"* (`notes/pkvm/evidence/finding-oom-leak-and-mmu-topup-oops-2026-07-30/ROOT-CAUSE-ANALYSIS.md:221`). It now has a deterministic reproducer. Note that document also states `handle_hyp_req_mem()` "accounts them into `kvm->stat.protected_hyp_mem`" — by the code in this tree it does not, which may be why the thread stopped there.

## Why this surfaced only now — and the part about issue 002

It was **not** exposed by fixing issue 002. Every observation above is on `#4`, which does not contain that fix; the fix is built as `#5` and is not deployed. Nor was it exposed by the issue 001 fix: the 001 reproducer exercises the code that fix changed, heavily, and produces none of these signatures.

What exposed it was the *requirement to verify* 002. Review established that 002's reproducer could never validate 002's fix, because it creates no vCPU: with `pkvm.handle` still 0, EL2 maps the handle to an out-of-range index and returns without flushing, so after the fix the warning would vanish because the call became a no-op. Verifying 002 therefore demanded a program that **runs a vCPU first** and only then enables dirty logging and resumes.

Nothing in this project had ever produced that sequence. 001's reproducer runs a vCPU but never touches dirty logging. 002's touches dirty logging but never runs a vCPU. The fuzzing campaigns enable both `ioctl$KVM_SET_USER_MEMORY_REGION` and `ioctl$KVM_RUN`, yet the 2026-07-31 and 2026-08-05 console logs contain zero instances of this signature — those runs never happened to compose the two against one VM in the required order. The first execution of that composition failed immediately and has failed every time since.

There is, separately, a real sense in which **fixing 002 will unmask 003 on hardware where it might otherwise hide**. Reaching the broken path requires the guest's write to actually fault. Before the 002 fix the write-protect is applied but the TLB invalidation that should make it visible is refused, so whether the guest faults depends on the CPU having evicted its stale writable entry. On N90 it faults anyway — that is exactly why `-E2BIG` is observable here on `#4` — but that is a property of this hardware and timing, not a guarantee. With 002 fixed the invalidation actually happens, and the fault, and therefore this failure, becomes reliable.

The order matters for anyone reading the tracker later: **002 does not cause 003, and 003 is not a regression from either of our patches.** 003 is pre-existing klinux behaviour that a new input shape reached for the first time, and 002's fix makes reaching it deterministic rather than incidental.

## Fix status

Not fixed. **There is nothing upstream to backport, because upstream does not have this defect** — it has a mechanism klinux deleted. Evidence: `evidence/2026-08-05-upstream-ack66.txt`, read from `kernel-refs/ack`.

**Upstream's EL2 handler needs no order, because the host gives it the `pfn`:**

```c
/* ACK android15-6.6, arch/arm64/kvm/hyp/nvhe/mem_protect.c:2771 */
int __pkvm_dirty_log(struct pkvm_hyp_vcpu *hyp_vcpu, u64 pfn, u64 gfn)
{
	u64 host_addr = hyp_pfn_to_phys(pfn);      /* straight from the host */
	...                                        /* no guest_get_valid_pte, no granule check */
	ret = kvm_pgtable_stage2_map(&vm->pgt, guest_addr, PAGE_SIZE, host_addr, ...);
```

klinux renamed it to `__pkvm_host_dirty_log_guest(gfn)`, **dropped the `pfn`**, and therefore had to re-derive `host_addr` at EL2 by walking the guest page table — which is where the 4 KiB-only `guest_get_valid_pte(..., 0, ...)` lookup came from.

**And upstream splits the block first, through machinery klinux does not have at all:**

| component | ACK `android15-6.6` | klinux |
| --- | --- | --- |
| `__pkvm_host_split_guest` hypercall | yes | **absent** |
| `__pkvm_pgtable_stage2_split()` host side | `mmu.c:1916` | **absent** |
| `handle_hyp_req_split()` | `handle_exit.c:366` | **absent** |
| `grep -rn split_guest arch/arm64/` | hits | **no matches** |

Upstream's comment above that function says it outright: *"`pkvm_pgtable_stage2_split()` can be called with dirty logging"* (`mmu.c:1912-1914`). EL2 meets a block, asks the host to split via a hyp request, the host pins the 511 sub-pages, calls `__pkvm_host_split_guest`, rewrites the pinned-page tracking to `order = 0` — and everything downstream then sees 4 KiB. That is why upstream's dirty-log handler can map `PAGE_SIZE` unconditionally.

Note that upstream is **not** simply a tree without huge pages: `struct kvm_pinned_page` carries `order` and `pins` in both trees.

So the fix is a restoration, with two possible shapes:

1. **Follow upstream** — restore the split path (`handle_hyp_req_split` + `__pkvm_pgtable_stage2_split` + the `__pkvm_host_split_guest` hypercall), so blocks never reach the dirty-log path. Largest change; matches upstream exactly, which matters if this tree is ever reconciled.
2. **Follow the siblings in this tree** — give the dirty-log hypercall an `order` argument like `__pkvm_host_wrprotect_guest` has, teach the EL2 handler to map `PAGE_SIZE << order`, and route the call through `pkvm_call_hyp_nvhe_ppage()` with a `__pkvm_dirty_log_call(pfn, gfn, order, args)` callback. Smaller, local, and the retry arm then handles an already-split block for free.

Shape 2 is the smaller patch; shape 1 is the one that stops this tree drifting further from ACK. Either way this is **local work with an upstream reference**, not a cherry-pick.

## Residual hazard

This blocks verification of issue 002 by its intended route. The plan was to prove the range TLB flush works by enabling dirty logging on a running guest and checking that post-write-protect writes land in the dirty bitmap — which cannot complete while the guest cannot resume. Either 003 is fixed first, or 002 is verified another way: an EL2-coverage capture showing `handle___pkvm_tlb_flush_vmid` executing, or the same dirty-bitmap check run under `MADV_NOHUGEPAGE`, which the matrix shows does resume.
