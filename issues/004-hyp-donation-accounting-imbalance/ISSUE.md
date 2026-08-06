---
id: 004
slug: hyp-donation-accounting-imbalance
title: The dirty-logging topup into stage2_mc is never accounted, so protected_hyp_mem ends negative and the hyp-donation leak check reports a bogus 18 EB
class: kernel-defect
signature: 'donations to the nVHE hyp are missing'
hazard: none
diagnosis: root-caused
disposition: open
repro: repro/probe-dirtylog-thp.c
observations:
  - target: 'klinux 6.6.103+ #4 @348c94763cc6'
    state: reproduced
    run: 2026-08-05-dirtylog-e2big
    evidence: evidence/2026-08-05-teardown-message.txt
---

# 004 — the hyp-donation leak check is broken, in both directions

中文版本:[ISSUE_zh.md](ISSUE_zh.md)

Every VM that enables dirty logging prints this at teardown:

```
kvm [35488]: 18446744073709543424B of donations to the nVHE hyp are missing
```

It reads as *18 exabytes of guest memory were donated to the hypervisor and never came back*. Every part of that reading is wrong: the quantity is 8 KiB, the sign is inverted, no memory was lost, and the check that produced it is the one thing in the tree meant to catch a genuine hyp-donation leak.

## Symptom

`arm.c:262-265`, at the end of `kvm_arch_destroy_vm()`:

```c
	if (atomic64_read(&kvm->stat.protected_hyp_mem))
		kvm_err("%lluB of donations to the nVHE hyp are missing\n",
			atomic64_read(&kvm->stat.protected_hyp_mem));
```

`atomic64_read()` returns a signed `s64`, printed with `%llu`:

```
18446744073709543424  −  2^64  =  −8192 bytes  =  −2 pages
```

So the counter is **negative by two pages**, and the message's wording — "missing", i.e. donated and not returned — describes the *opposite* direction. A real leak leaves a **positive** residual.

## Mechanism

**Confidence: root-caused.** The imbalance is measured, its trigger is isolated by a controlled matrix, and the code path accounts for it.

**The counter tracks pages the host has handed to EL2.** Two sites add:

```
mmu.c:1801   pkvm_mem_abort()        atomic64_add(nr_pages << PAGE_SHIFT, ...)  /* what its topup consumed */
pkvm.c:401   __pkvm_create_hyp_vm()  atomic64_add(pgd_sz, ...)
```

Two sites subtract, and both subtract **the entire remaining memcache**, regardless of which path put pages in it:

```
arm.c:509    kvm_arch_vcpu_destroy()  atomic64_sub(vcpu->arch.stage2_mc.nr_pages << PAGE_SHIFT, ...)
pkvm.c:347   pkvm_destroy_hyp_vm()    atomic64_sub(stage2_teardown_mc.nr_pages << PAGE_SHIFT, ...)
```

**`__topup_hyp_memcache()` never accounts** (`kvm_host.h:119-135` — it only allocates and pushes), so accounting is entirely the caller's job. Exactly three sites top up `vcpu->arch.stage2_mc`, and only one of them does it:

| site | context | accounts? |
| --- | --- | --- |
| `mmu.c:1796` | `pkvm_mem_abort()`, the ordinary fault path | **yes**, `mmu.c:1801` |
| `mmu.c:1754` | `pkvm_relax_perms()`, the `logging_active` branch | **no** |
| `handle_exit.c:369` | `handle_hyp_req_mem()`, EL2 asking the host for memory | **no** |

Pages that enter through an unaccounted path and are still in the memcache at destroy are subtracted without ever having been added. The counter goes negative by exactly that many pages.

**The trigger is isolated, not argued.** Three modes, five runs each (`../003-dirty-log-guest-no-huge-page-support/evidence/2026-08-05-mode-matrix.txt`):

| mode | dirty logging | huge pages | `KVM_RUN` | this message |
| --- | --- | --- | --- | --- |
| default | on | 2 MiB block | fails (issue 003) | **yes** |
| `nohuge` | on | forced 4 KiB | succeeds | **yes** |
| `nodirty` | off | 2 MiB block | succeeds | **no**, 0/5 |

Enabling dirty logging is the only thing that matters. Page size does not, and neither does whether `KVM_RUN` then fails.

That last column is what points at `mmu.c:1754` specifically: it is the topup **inside** the `logging_active` branch, and it runs *before* the hypercall that issue 003 makes fail —

```c
	if (logging_active) {
		struct kvm_hyp_memcache *hyp_memcache = &vcpu->arch.stage2_mc;
		int ret = topup_hyp_memcache(hyp_memcache,              /* mmu.c:1754 — unaccounted */
					     kvm_mmu_cache_min_pages(&kvm->arch.mmu), 0);
		if (ret)
			return ret;

		ret = kvm_call_hyp_nvhe(__pkvm_host_dirty_log_guest, gfn);  /* 003 fails here */
```

— which is exactly why the imbalance survives both outcomes.

**One step is inference, not measurement:** that the two pages come from `mmu.c:1754` rather than from `handle_exit.c:369` is by elimination — `:1754` is the only unaccounted topup that is *reached only when dirty logging is on*. Instrumenting the three topup sites with counters, or exposing `stage2_mc.nr_pages` through debugfs, would settle it.

## Trigger

Any ordinary VM on a `kvm-arm.mode=protected` host that runs a vCPU and then has `KVM_MEM_LOG_DIRTY_PAGES` enabled on a memslot. No huge pages required.

## Reproduction

`repro/probe-dirtylog-thp.c` — the same program as issue 003, whose `nohuge` mode isolates this issue cleanly:

```
./probe-dirtylog-thp nohuge     # KVM_RUN succeeds, and this message still appears
./probe-dirtylog-thp nodirty    # no dirty logging, and it does not
```

The message is printed from `kvm_arch_destroy_vm()`, i.e. **after the process exits**. Reading `dmesg` synchronously after the program returns misses it intermittently — see the run record; two earlier passes were read that way and produced a wrong conclusion.

## Blast radius

**No memory is lost, and the code settles that more firmly than a measurement could.** The site that subtracts is the site that frees:

```c
	atomic64_sub(vcpu->arch.stage2_mc.nr_pages << PAGE_SHIFT,   /* arm.c:509  */
		     &vcpu->kvm->stat.protected_hyp_mem);
	free_hyp_memcache(&vcpu->arch.stage2_mc);                   /* arm.c:511  */
```

and `__free_hyp_memcache()` pops and frees every page in the cache (`kvm_host.h:137-149`). Every page counted out at teardown is a page handed back. The subtraction is correct; it is the matching *addition* that never happened, which is exactly why the residual is negative rather than positive.

A direct memory measurement was attempted and **could not resolve this**, recorded in `evidence/2026-08-05-memory-ab.txt` so that nobody repeats it. Across 300 VM cycles per arm, with `drop_caches` at every boundary, `MemFree` deltas swing by ±55 MB in both directions in both arms, while a two-page-per-VM loss would be 2400 kB — an order of magnitude below the noise. An earlier attempt without `drop_caches` produced two mutually contradictory repetitions. The most that measurement supports is the absence of a *large* leak.

What is damaged is **the leak detector itself**, and that is the reason this is filed rather than dismissed as cosmetic:

- **A genuine leak through the unaccounted channels would be invisible.** `handle_hyp_req_mem()` and the dirty-logging topup hand pages to EL2 without the stat ever knowing. If those pages were never returned, `protected_hyp_mem` would not rise, and this check would stay silent.
- **The detector now cries wolf.** Every dirty-logging VM prints an alarming, wrong 18 EB at teardown, so a real report will not be believed.

## Relationship to the other issues

All three of 002, 003 and 004 fire on the same input — an ordinary VM whose vCPU has run, then dirty logging enabled — which is why they were found together. They are **not** the same defect, and the mode matrix separates them experimentally rather than by argument:

| | issue 002 | issue 003 | **issue 004** |
| --- | --- | --- | --- |
| what | range TLB flush asks EL2 for a retired hypercall | dirty-log path assumes a 4 KiB mapping | dirty-log topup is unaccounted |
| needs dirty logging | yes | yes | yes |
| needs huge pages | **no** | **yes** | **no** |
| survives `KVM_RUN` succeeding | yes | n/a (it *is* the failure) | **yes** |
| where | host, `mmu.c:190` | EL2, `permissions.rs:147` | host, `mmu.c:1754` |
| effect | TLB invalidation silently never runs | guest cannot resume | a counter is wrong |

`nohuge` mode is the discriminator: `KVM_RUN` succeeds, so issue 003 is absent, and 002 and 004 both still appear. `nodirty` removes all three.

**003 and 004 are neighbours in the same function.** `pkvm_relax_perms()`'s `logging_active` branch contains both: the unaccounted topup at `:1754` (this issue) and, two lines later, the hypercall that 003 makes fail. Both look like the same omission — a branch written without the treatment its siblings received — but they are independent: fixing either leaves the other.

**Neither is a regression from our own patches**, and 004 is not caused by 003. Every observation is on `#4`, which carries the issue 001 fix and not the 002 fix. See 003's "Why this surfaced only now" for the full account: what exposed all of this was the requirement to *verify* 002, which forced writing the first program in this project to run a vCPU and then enable dirty logging.

**Prior art in this tree.** The 2026-07-30 OOM investigation named the untracked host→EL2 donation channel as its most promising remaining candidate and recorded it as *"Unverified — no evidence gathered yet"* (`notes/pkvm/evidence/finding-oom-leak-and-mmu-topup-oops-2026-07-30/ROOT-CAUSE-ANALYSIS.md:221`, `ROOT-CAUSE-CONFIRMED.md:274`). This issue is that channel, with a deterministic reproducer — but note what it does and does not settle: it proves the channel exists and does not account, and it explicitly does **not** explain that investigation's 25.9 GiB residual, whose direction and magnitude are both wrong for this. That document also states `handle_hyp_req_mem()` "accounts them into `kvm->stat.protected_hyp_mem`"; by the code in this tree it does not, which may be why the thread stopped there.

## Fix status

Not fixed. Two independent changes, both host-side only — no EL2 ABI is involved, which makes this substantially simpler than 003:

1. **Account the topups.** Add the `atomic64_add` at `mmu.c:1754` mirroring `mmu.c:1801`, and at `handle_exit.c:369`. The pattern at `pkvm_mem_abort()` is to record the *increase* in `nr_pages` across the topup, not the requested minimum.
2. **Fix the report.** `arm.c:263-265` should print the signed value, and distinguish the two directions: a positive residual is memory donated and never returned, a negative one is an accounting bug. Conflating them is what made this message unreadable.

Upstream has not been searched for either yet — that is step 2 of the issue workflow and is still outstanding.

## Residual hazard

Fixing the report without fixing the accounting would silence the message and leave the detector blind. Fixing the accounting without auditing the other donation channels leaves whatever else is untracked still untracked: `__pkvm_topup_hyp_alloc()` (`pkvm.c:1243`) tops up a local memcache and does not account either, and it was not exercised by this reproducer.
