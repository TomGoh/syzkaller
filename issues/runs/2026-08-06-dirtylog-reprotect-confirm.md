---
id: 2026-08-06-dirtylog-reprotect-confirm
kind: reproduction
board: N90 (10.42.27.17)
kernel: '6.6.103+ #4 SMP Wed Aug 5 11:28:51 — klinux pkvm-unmap-deadlock-fix @348c94763cc6'
cmdline: kvm-arm.mode=protected
manager_rev: n/a (no syz-manager involved)
config: n/a
filters:
  config_ignores: n/a
  reporter_ignores: n/a
enabled_syscalls: n/a
duration: ~3m, one traced run plus five repeats
result: 'the re-write-protect after a successful dirty-log returns -EPERM 5/5 where the first write-protect of the same page returned 0, and the page''s next write is lost from the dirty bitmap 5/5'
ring: n/a
observed: [5]
not_observed: []
---

# Run 2026-08-06-dirtylog-reprotect-confirm

A prediction test, not an exploration. The upstream/code work on 2026-08-06 concluded that the EL2 dirty-log handler's bare-prot `kvm_pgtable_stage2_map()` zeroes the guest PTE's page-state bits. That was a code read of two trees and nothing more, so per the project rule it could not be used to justify a patch. This run was designed to falsify it.

## What was predicted before running

If the state really is zeroed, then `__pkvm_host_wrprotect_guest()` — which calls the same `__check_host_unshare_guest()` as the teardown unmap — must start failing on any page that has been through a successful dirty log. That path is reachable from userspace: `KVM_GET_DIRTY_LOG` re-write-protects the pages it reports. And because `kvm_stage2_wp_range()` is `void`, the error would be dropped, leaving the page writable, so the guest's next write would take **no fault at all** and `mark_page_dirty_in_slot()` would never run.

Falsifiable form: **page A's second write under dirty logging must be missing from the bitmap.**

`repro/probe-dirtylog-twice.c` was written for this, with a second page B as an in-run control — faulted in at the same moment, but not dirty-logged in round 2, and therefore not re-protected by `KVM_GET_DIRTY_LOG` (the mask only covers pages reported dirty).

## Result

Confirmed, 5/5. Full trace and table in `../005-unmap-guest-fails-after-dirty-log/evidence/2026-08-06-reprotect-eperm-and-lost-dirty-page.txt`.

| | 5/5 |
| --- | --- |
| round 2 tracks A correctly (`A=1`) | yes — the bitmap plumbing works |
| initial full-slot write-protect | succeeds, all three pages `ret=0` |
| re-write-protect after the dirty log | **`-EPERM`, never `0`** |
| A's round-3 write in the bitmap | **absent** |
| teardown unmap | `-EPERM` — issue 005's original signature |

The decisive part is a before/after on **the same page and the same hypercall**, about 500 µs apart:

```
PHASE enable-dirty-logging
  ppage_ret (__stage2_wp_range <- ...) ret=0   x3      <- code page, A and B
PHASE getdirty-1-reprotect
  ppage_ret (__stage2_wp_range <- ...) ret=4294967295  <- page A alone, -EPERM
  wpr_ret   (kvm_arch_mmu_enable_log_dirty_pt_masked)  ret=4294967295
```

`__pkvm_host_wrprotect_guest` passes on A, then fails on A. The only thing between them is one successful `__pkvm_host_dirty_log_guest`. That kills "the check was always going to fail here" and any static property of the page.

It also narrows which of the check's two `-EPERM` sites fires. The dirty-log handler writes only `vm->pgt`; nothing on that path touches host state or the vmemmap, so the host-side comparison cannot have changed between the passing and failing calls. The guest-state comparison is what is left. Still inferred rather than read out of the PTE — the EL2 coverage capture would do that — but no longer a coin flip between two candidates.

## What the empty phases prove

`round2-run` and `round3-run` contain **no probe hits at all**, and both absences are informative:

- Round 2: A's permission fault takes `pkvm_relax_perms()`'s `logging_active` branch, a bare `kvm_call_hyp_nvhe(__pkvm_host_dirty_log_guest, gfn)` at `mmu.c:1755` that does not go through `pkvm_call_hyp_nvhe_ppage()`. Being a permission fault, it does not reach `pkvm_mem_abort` either.
- Round 3: A takes no fault whatsoever. That *is* the defect.

## Severity — larger than the issue was filed for

005 was filed on a teardown WARN with the board unaffected. This run shows the same corruption also breaks dirty logging itself, silently: after the first write to a page, that page can never be re-write-protected, so every later write to it goes unrecorded, and `kvm_arch_mmu_enable_log_dirty_pt_masked()` being `void` means nothing is reported anywhere. A migration or snapshot taken this way copies stale data and reports success.

## Method

Three probes, with `tracing_on` set to `1` first — probes install and enable without complaint and record nothing when it is `0`, which cost a full cycle on 2026-08-05:

```
r:ppage_ret pkvm_call_hyp_nvhe_ppage ret=$retval:s64
r:wpr_ret   __stage2_wp_range        ret=$retval:s64
p:memabort  pkvm_mem_abort
```

`pkvm_relax_perms`, `pkvm_wp_range` and `user_mem_abort` are all inlined on this build and absent from `/proc/kallsyms`. What makes the trace readable anyway is the kretprobe's caller annotation — `(__stage2_wp_range+0x11c <- pkvm_call_hyp_nvhe_ppage)` versus `(__unmap_stage2_range+0x1e8 <- ...)` — which separates the write-protect path from the unmap path without needing those symbols. The reproducer writes phase names to `trace_marker` so each return can be attributed to a round; without that the trace is a list of numbers.

## Honest gap

The control page B is tracked in 3/5 runs and lost in 2/5. Nothing above depends on it — A fails 5/5 either way — and no explanation is offered here. A candidate worth remembering: `pkvm_relax_perms()` can return `-EAGAIN`, which `user_mem_abort()` turns into `0` while still skipping `mark_page_dirty_in_slot()` under `if (writable && !ret)`. That would be a second, unrelated way to drop a dirty bit, and it is not investigated.

## Scope

Says nothing about issue 001 (no second `KVM_ARM_VCPU_INIT`), and nothing about 002. Issue 003 does not fire because the run is `nohuge` throughout; that is a precondition of the test rather than an observation about 003.
