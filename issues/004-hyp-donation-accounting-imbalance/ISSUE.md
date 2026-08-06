---
id: 004
slug: hyp-donation-accounting-imbalance
title: The dirty-logging topup into stage2_mc is never accounted, so protected_hyp_mem ends negative and the hyp-donation leak check reports a bogus 18 EB
class: kernel-defect
signature: 'donations to the nVHE hyp are missing'
hazard: none
diagnosis: root-caused
disposition: fix-verified
repro: repro/probe-dirtylog-thp.c
observations:
  - target: 'klinux 6.6.103+ #4 @348c94763cc6'
    state: reproduced
    run: 2026-08-05-dirtylog-e2big
    evidence: evidence/2026-08-05-teardown-message.txt
  - target: 'klinux 6.6.103+ #13 @24714fe308bb'
    state: not-observed
    run: 2026-08-06-fix-deploy-verify
---

# 004 — the hyp-donation leak check is broken, in both directions

中文版本:[ISSUE_zh.md](ISSUE_zh.md)

> **Line numbers in this document are on `klinux @348c94763cc6` (`#4`)**, the build every observation was made on — read them with `git show 348c94763cc6:<path>`. An earlier revision cited the working tree, which carries the issue 002 fix and is +5 lines through `mmu.c`.

Every VM that enables dirty logging prints this at teardown:

```
kvm [35488]: 18446744073709543424B of donations to the nVHE hyp are missing
```

It reads as *18 exabytes of guest memory were donated to the hypervisor and never came back*. Every part of that reading is wrong: the quantity is 8 KiB, the sign is inverted, no memory was lost, and the check that produced it is the one thing in the tree meant to catch a genuine hyp-donation leak.

## Symptom

`arm.c:263-265`, at the end of `kvm_arch_destroy_vm()`:

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

**The counter tracks pages the host has handed to EL2.** Three sites add and four subtract:

```
add   mmu.c:1796     pkvm_mem_abort()          what its own topup consumed
add   pkvm.c:401     __pkvm_create_hyp_vm()    pgd_sz
add   alloc.rs:1648  hyp_alloc_account()       EL2-side allocation   (C mirror: nvhe/alloc.c:626)

sub   arm.c:511      kvm_arch_vcpu_destroy()   the whole of stage2_mc.nr_pages
sub   pkvm.c:347     pkvm_destroy_hyp_vm()     the whole of stage2_teardown_mc.nr_pages
sub   pkvm.c:431     __pkvm_create_hyp_vm()    pgd_sz, on the create-failure rollback
sub   alloc.rs:1729  hyp_free_account()        EL2-side free         (C mirror: nvhe/alloc.c:661)
```

Two of those are self-balancing pairs — the pgd add/rollback, and the EL2 allocator's own add/free — and neither participates in the imbalance. An earlier revision listed only two adds and two subs; the conclusion is unchanged, but the enumeration was incomplete because both missed pairs span two source lines and a single-line `grep` for `atomic64_add.*protected_hyp_mem` does not see them.

**The two teardown subtractions take the entire remaining memcache**, regardless of which path put pages into it. That is the half of the asymmetry that does the damage.

**`__topup_hyp_memcache()` never accounts** (`kvm_host.h:119-135` — it only allocates and pushes), so accounting is entirely the caller's job. Exactly three sites top up `vcpu->arch.stage2_mc`, and only one of them does it:

| site | context | accounts? |
| --- | --- | --- |
| `mmu.c:1791` | `pkvm_mem_abort()`, the ordinary fault path | **yes**, `mmu.c:1796` |
| `mmu.c:1749` | `pkvm_relax_perms()`, the `logging_active` branch | **no** |
| `handle_exit.c:369` | `handle_hyp_req_mem()`, EL2 asking the host for memory | **no** |

Pages that enter through an unaccounted path are subtracted without ever having been added, and the counter goes negative by exactly that many.

Note the precondition is weaker than "still in the memcache at destroy", which an earlier revision claimed. Pages EL2 *consumed* go negative too: they come back at teardown through `stage2_teardown_mc` and are subtracted there (`pkvm.c:347`). Either way the subtraction is real and the addition never happened.

**The magnitude is predicted, not just observed.** `kvm_mmu_cache_min_pages(mmu)` is `kvm_stage2_levels(mmu) - 1` (`stage2_pgtable.h:31`), and the logging topup asks for exactly that. With this board's 40-bit IPA giving three stage-2 levels, it is **2** — matching the measured −8192 bytes exactly. That closes what the first revision left as an open question.

**The trigger is isolated, not argued.** Three modes, five runs each (`../003-dirty-log-guest-no-huge-page-support/evidence/2026-08-05-mode-matrix.txt`):

| mode | dirty logging | huge pages | `KVM_RUN` | this message |
| --- | --- | --- | --- | --- |
| default | on | 2 MiB block | fails (issue 003) | **yes** |
| `nohuge` | on | forced 4 KiB | succeeds | **yes** |
| `nodirty` | off | 2 MiB block | succeeds | **no**, 0/5 |

Page size does not matter, and neither does whether `KVM_RUN` then fails.

**But "enabling dirty logging" is too loose a trigger**, as an earlier revision put it. Per-run correlation over 15 runs gives 13/15 `-E2BIG` and 13/15 teardown messages, coinciding exactly; the two runs in which the guest did not fault produced neither. What the message actually requires is that the **logging fault path executes** — which strengthens the causal claim, because that path is precisely where `mmu.c:1749` sits.

That last column is what points at `mmu.c:1749` specifically: it is the topup **inside** the `logging_active` branch, and it runs *before* the hypercall that issue 003 makes fail —

```c
	if (logging_active) {
		struct kvm_hyp_memcache *hyp_memcache = &vcpu->arch.stage2_mc;
		int ret = topup_hyp_memcache(hyp_memcache,              /* mmu.c:1749 — unaccounted */
					     kvm_mmu_cache_min_pages(&kvm->arch.mmu), 0);
		if (ret)
			return ret;

		ret = kvm_call_hyp_nvhe(__pkvm_host_dirty_log_guest, gfn);  /* 003 fails here */
```

— which is exactly why the imbalance survives both outcomes.

**One step is inference, not measurement:** that the two pages come from `mmu.c:1749` rather than from `handle_exit.c:369` is by elimination — `:1749` is the only unaccounted topup that is *reached only when dirty logging is on*. Instrumenting the three topup sites with counters, or exposing `stage2_mc.nr_pages` through debugfs, would settle it.

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
	atomic64_sub(vcpu->arch.stage2_mc.nr_pages << PAGE_SHIFT,   /* arm.c:511  */
		     &vcpu->kvm->stat.protected_hyp_mem);
	free_hyp_memcache(&vcpu->arch.stage2_mc);                   /* arm.c:513  */
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
| where | host, `mmu.c:190` | EL2, `permissions.rs:147` | host, `mmu.c:1749` |
| effect | TLB invalidation silently never runs | guest cannot resume | a counter is wrong |

`nohuge` mode is the discriminator: `KVM_RUN` succeeds, so issue 003 is absent, and 002 and 004 both still appear. `nodirty` removes all three.

**003 and 004 are neighbours in the same function.** `pkvm_relax_perms()`'s `logging_active` branch contains both: the unaccounted topup at `:1749` (this issue) and, six lines later, the hypercall that 003 makes fail. Both look like the same omission — a branch written without the treatment its siblings received — but they are independent: fixing either leaves the other.

**Neither is a regression from our own patches**, and 004 is not caused by 003. Every observation is on `#4`, which carries the issue 001 fix and not the 002 fix. See 003's "Why this surfaced only now" for the full account: what exposed all of this was the requirement to *verify* 002, which forced writing the first program in this project to run a vCPU and then enable dirty logging.

**Prior art in this tree.** The 2026-07-30 OOM investigation named the untracked host→EL2 donation channel as its most promising remaining candidate and recorded it as *"Unverified — no evidence gathered yet"* (`notes/pkvm/evidence/finding-oom-leak-and-mmu-topup-oops-2026-07-30/ROOT-CAUSE-ANALYSIS.md:221`, `ROOT-CAUSE-CONFIRMED.md:274`). This issue is that channel, with a deterministic reproducer — but note what it does and does not settle: it proves the channel exists and does not account, and it explicitly does **not** explain that investigation's 25.9 GiB residual, whose direction and magnitude are both wrong for this. That document also states `handle_hyp_req_mem()` "accounts them into `kvm->stat.protected_hyp_mem`"; by the code in this tree it does not, which may be why the thread stopped there.

## Fix status

> **Verified on hardware 2026-08-06** (`#13`, klinux `61173329e416` + `f8ac14623978`, run [2026-08-06-fix-deploy-verify](../runs/2026-08-06-fix-deploy-verify.md)): after five THP dirty-logging runs plus a `probe-dirtylog-twice` run, `dmesg` carries **no** donation message in either wording. The check only prints on a non-zero residual, so zero messages means the accounting balances.

> **Fixes are committed in klinux as `61173329e416`** (parts a and c — restore the two missing `atomic64_add()` calls) **and `f8ac14623978`** (part b — print the residual signed and say which direction). Compile-tested only — `disposition: fix-proposed`, not verified. The check is that the teardown message stops appearing at all.

Fixed locally as above. Upstream **has** been searched, against `kernel-refs/ack` (`aosp/android15-6.6`, `android16-6.12`, `android17-6.18`); evidence in `evidence/2026-08-05-upstream-ack.txt`. The answer differs per part, so this issue is three defects with three different provenances:

### (a) `handle_hyp_req_mem()` — klinux **deleted** upstream's accounting

Upstream carries exactly the code that is missing here:

```c
/* ACK android15-6.6, arch/arm64/kvm/handle_exit.c:344-352 */
	case REQ_MEM_DEST_VCPU_MEMCACHE:
		nr_pages = vcpu->arch.stage2_mc.nr_pages;
		ret = topup_hyp_memcache(&vcpu->arch.stage2_mc, req->mem.nr_pages, 0);
		nr_pages = vcpu->arch.stage2_mc.nr_pages - nr_pages;
		atomic64_add(nr_pages << PAGE_SHIFT, &kvm->stat.protected_hyp_mem);
		atomic64_add(nr_pages << PAGE_SHIFT, &kvm->stat.protected_pgtable_mem);
		return ret;

/* klinux @348c94763cc6, arch/arm64/kvm/handle_exit.c:365-371 */
	case REQ_MEM_DEST_VCPU_MEMCACHE:
		return topup_hyp_memcache(&vcpu->arch.stage2_mc, req->mem.nr_pages, 0);
```

The delta computation and both adds were dropped. `protected_pgtable_mem` does not exist anywhere in `klinux/arch/arm64/` — the second stat went with it. **This is a klinux regression with an exact upstream reference; the fix is to restore the upstream form.**

### (b) The `%llu`-on-signed report — an **upstream** defect, unfixed everywhere

Byte-identical in all three ACK branches, including the newest:

```
aosp/android15-6.6   arm.c:235
aosp/android16-6.12  arm.c:267
aosp/android17-6.18  arm.c:284
	kvm_err("%lluB of donations to the nVHE hyp are missing\n",
		atomic64_read(&kvm->stat.protected_hyp_mem));
```

Nothing to backport. The fix is local, and this one is **worth reporting upstream**: print the signed value, and distinguish the directions — a positive residual is memory donated and never returned, a negative one is an accounting bug. Conflating them is what made this message unreadable.

### (c) The logging-branch topup — **upstream is also unaccounted**

ACK 6.6 has the same gap at its own `mmu.c:1670`: the `logging_active` topup is followed straight by the hypercall, with no `atomic64_add`. Upstream is inconsistent with itself here, since the block-splitting path two hundred lines below (`mmu.c:1931-1938`) *does* account its topup. So this half is inherited and unfixed upstream, and fixing it locally means diverging — mirror `pkvm_mem_abort()`'s `nr_pages`-delta pattern.

### Order of work

(a) is the cheapest and the only one with an upstream answer to copy. (b) is two lines. (c) is the one that actually removes the −2 pages, and is best done together with (a) so the accounting is complete rather than half-restored. None of the three touches the EL2 ABI, which makes all of this substantially simpler than 003.

## Residual hazard

Fixing the report without fixing the accounting would silence the message and leave the detector blind. Fixing the accounting without auditing the other donation channels leaves whatever else is untracked still untracked: `__pkvm_topup_hyp_alloc()` (`pkvm.c:1243`) tops up a local memcache and does not account either, and it was not exercised by this reproducer.
