---
id: 005
slug: unmap-guest-fails-after-dirty-log
title: Once the dirty-logging hypercall succeeds, the EL2 guest unmap fails at VM teardown and WARNs from exit_mmap
class: kernel-defect
signature: 'WARNING in __unmap_stage2_range'
hazard: none
diagnosis: hypothesis
disposition: open
repro: repro/probe-dirtylog-thp.c
observations:
  - target: 'klinux 6.6.103+ #4 @348c94763cc6'
    state: reproduced
    run: 2026-08-05-unmap-warn-verify
    evidence: evidence/2026-08-05-mode-matrix-and-instance.txt
---

# 005 — the guest unmap fails at teardown, but only after dirty logging worked

中文版本:[ISSUE_zh.md](ISSUE_zh.md)

> **Line numbers are on `klinux @348c94763cc6` (`#4`)**, the build this was observed on — `git show 348c94763cc6:<path>`.

A VM whose dirty-logging hypercall **succeeded** cannot be torn down cleanly: the stage-2 unmap returns an error, and `__unmap_stage2_range()` warns about it from inside `exit_mmap()`. It is currently invisible, because issue 003 makes that hypercall fail on every huge-page-backed guest — which is every guest, by default.

Filed now rather than later because of that dependency: **fixing 003 will make this appear on every dirty-logging teardown**, where it will look like a regression from the 003 fix. It is not; it is older than the fix and merely hidden by it.

## Symptom

```
WARNING: CPU: 7 PID: 94783 at arch/arm64/kvm/mmu.c:456 __unmap_stage2_range+0x26c/0x2b8
CPU: 7 PID: 94783 Comm: probe3 Tainted: G        W          6.6.103+ #4
Call trace:
 __unmap_stage2_range+0x26c/0x2b8
 kvm_uninit_stage2_mmu+0x6c/0xe0
 kvm_arch_flush_shadow_all+0x20/0x30
 kvm_mmu_notifier_release+0x38/0x98
 __mmu_notifier_release+0x90/0x2b0
 exit_mmap+0x46c/0x4c8
 __mmput+0x48/0x1e8
 do_exit+0x388/0xcb0
 __arm64_sys_exit_group+0x28/0x30
```

`mmu.c:456` is the return check on the unmap walk itself:

```c
	lockdep_assert_held_write(&kvm->mmu_lock);
	WARN_ON(size & ~PAGE_MASK);                                    /* :455 */
	WARN_ON(stage2_apply_range(mmu, start, end, ___unmap_stage2_range,
				   may_block));                        /* :456 */
```

The board is unaffected: no hang, no Oops, and the process exits normally. What is lost is the guarantee the warning exists to protect — that a VM's EL2 mappings are all released when it dies.

## Trigger, isolated

Three modes, five runs each, on `#4` (`evidence/2026-08-05-mode-matrix-and-instance.txt`):

| mode | guest memory | dirty logging | `-E2BIG` (003) | **this warning** |
| --- | --- | --- | --- | --- |
| default | 2 MiB THP block | on | 5/5 | **0/5** |
| `nohuge` | forced 4 KiB | on | 0/5 | **5/5** |
| `nodirty` | 2 MiB THP block | off | 0/5 | **0/5** |

**Perfectly anti-correlated with issue 003.** The warning fires exactly when the dirty-logging hypercall succeeds, and never when it fails or is never made. Dirty logging is necessary — `nodirty` is a hard zero — but not sufficient: the call must also have *gone through*.

## Mechanism

**Confidence: hypothesis.** Which call fails is established; *why* it fails is not.

**What fails is the EL2 guest-unmap hypercall.** `pkvm_unmap_range()`, `___unmap_stage2_range()` and `stage2_apply_range()` are all inlined into `__unmap_stage2_range()`, so the warning's `brk` is reached directly from the failing call (`evidence/2026-08-05-warn-site-disasm.txt`):

```
0x8c4  bl   pkvm_call_hyp_nvhe_ppage      /* mmu.c:325, __pkvm_unmap_guest_call */
0x8c8  cbz  w0, +0x1f4                    /* w0 is the error */
0x8cc  bl   __sanitizer_cov_trace_pc
0x8d0  b    +0x268  ->  brk at +0x26c     /* the WARN */
```

**The error code is not recoverable from the register dump**, and this is worth stating because the same technique settled issue 002. `w0` holds it at `0x8c8`, but the SanCov call at `0x8cc` clobbers `x0` before the `brk` — the `#4` dump duly shows `x0 : 0`. Getting the value needs a kretprobe on `pkvm_call_hyp_nvhe_ppage()` or a temporary `WARN_ONCE` printing `err`.

**It is not the inner range check.** `pkvm_unmap_range()` has its own `WARN_ON(end < ppage->ipa + (PAGE_SIZE << ppage->order))` at `mmu.c:364`, which would have printed separately. It did not appear in any capture, so the error came back from EL2 rather than from the host's own walk.

**Candidate error paths.** `pkvm_call_hyp_nvhe_ppage()` (`pkvm.c:240-296`) absorbs most failures, so only four returns can reach the caller:

| return | condition |
| --- | --- |
| `-EINVAL` `pkvm.c:263` | EL2 said `-E2BIG` while `order == 0` — "something is really wrong" |
| `-EINVAL` `pkvm.c:272` | EL2 said `-ENOENT` for a page whose `order` is 0 — "not supposed to lose track of a PAGE_SIZE pinned page" |
| `-EINVAL` `pkvm.c:281` | `page_size > size` |
| `err` `pkvm.c:293` | any other EL2 error, verbatim |

**Why the anti-correlation is suggestive.** In `nohuge` the pages are 4 KiB, so `ppage->order == 0` — and both of the first two rows are exactly the arms that turn a *recoverable* EL2 answer into a hard `-EINVAL` when the order is already zero. In `default` mode the same EL2 answer would be retried or fallen through. That is a plausible reading of why the warning tracks the page size, but it is **not established**: it assumes the EL2 error is `-E2BIG` or `-ENOENT`, which is exactly the value that could not be read.

**What the successful dirty-logging call leaves behind.** When it works, `__pkvm_host_dirty_log_guest` runs `check_unshare()` and then re-maps the page `KVM_PGTABLE_PROT_RWX` (`permissions.rs:135-211`). So it mutates EL2 page state and the guest stage-2 entry, and the later unmap of that same page is what fails. When issue 003 aborts the call with `-E2BIG`, none of that mutation happens — which is the shape of the masking, and the reason this could not have been seen before.

## Reproduction

`repro/probe-dirtylog-thp.c` in `nohuge` mode:

```
aarch64-linux-gnu-gcc -O2 -static -o probe-dirtylog-thp repro/probe-dirtylog-thp.c
./probe-dirtylog-thp nohuge     # KVM_RUN succeeds, and this warning follows at exit
./probe-dirtylog-thp            # THP: issue 003 fires instead, and this one does not
./probe-dirtylog-thp nodirty    # neither
```

The warning is emitted at process exit, through `exit_mmap`. Read `dmesg` after a settle — reading it synchronously after the program returns misses it intermittently.

## Blast radius

The unmap walk aborts where it fails. `stage2_apply_range()` returns the first error, so any pinned pages after that point in the range are not unmapped, not unpinned, and not accounted — for a VM that is being destroyed. Whether that is a real leak depends on what `__pkvm_finalize_teardown_vm` and `drain_hyp_pool` recover afterwards, which has not been checked.

No memory-loss measurement has been attempted for this issue. The lesson from issue 004 applies: `MemFree` at this scale is dominated by noise, so the question is better settled by following the teardown code than by weighing the machine.

## Relationship to the other issues

All four fire on the same input — an ordinary VM whose vCPU has run, then dirty logging enabled — and the mode matrix separates them experimentally:

| | 002 | 003 | 004 | **005** |
| --- | --- | --- | --- | --- |
| needs dirty logging | yes | yes | yes | yes |
| needs huge pages | no | **yes** | no | **no — the opposite** |
| needs the dirty-log call to *succeed* | no | n/a | no | **yes** |
| where | host `mmu.c:190` | EL2 `permissions.rs:147` | host `mmu.c:1749` | EL2 unmap, via `mmu.c:325` |

**They mask each other in a chain**, which is the practical reason to file this now:

- **002 masks 003.** Without the TLB invalidation the write-protect may not be visible to the guest, so the fault that reaches the broken path is not guaranteed — which is why 003 measures 22/25 rather than 25/25 on `#4`.
- **003 masks 005.** The dirty-log hypercall aborts before EL2 mutates any state, so the unmap that would fail never gets the chance.

Fixing them in order therefore *reveals* rather than resolves: fix 002 and 003 goes to 100 %; fix 003 and **005 appears on every dirty-logging teardown**. Anyone who lands the 003 patch and then sees this warning should read it as an unmasking, not a regression.

## Fix status

Not fixed, and not diagnosed far enough to propose one. The next step is the error code, which is one kretprobe away.

**Upstream has not been searched for this one yet.** Note that the search will be shaped by what issue 003 already established: `__pkvm_host_dirty_log_guest` does not exist upstream at all — ACK uses `__pkvm_dirty_log(pfn, gfn)` and splits blocks beforehand — so the state this warning complains about may simply never arise there. That would make 005, like 003, local rather than inherited; it is not yet checked.

## Residual hazard

Because 003 hides this, any measurement of 005's frequency taken on a `#4`-like kernel is a lower bound, and any campaign that never enables dirty logging will never see it at all. The syzkaller campaigns to date fall in that second category — the signature does not appear in the 2026-07-31 or 2026-08-05 console logs.
