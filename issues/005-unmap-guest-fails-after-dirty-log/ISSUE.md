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

**Confidence: hypothesis.** Which call fails, and with what error, are both measured. *Why EL2 refuses* is not — and cannot be reached with the instruments used so far.

**What fails is the EL2 guest-unmap hypercall.** `pkvm_unmap_range()`, `___unmap_stage2_range()` and `stage2_apply_range()` are all inlined into `__unmap_stage2_range()`, so the warning's `brk` is reached directly from the failing call (`evidence/2026-08-05-warn-site-disasm.txt`):

```
0x8c4  bl   pkvm_call_hyp_nvhe_ppage      /* mmu.c:325, __pkvm_unmap_guest_call */
0x8c8  cbz  w0, +0x1f4                    /* w0 is the error */
0x8cc  bl   __sanitizer_cov_trace_pc
0x8d0  b    +0x268  ->  brk at +0x26c     /* the WARN */
```

**It is not the inner range check.** `pkvm_unmap_range()` has its own `WARN_ON(end < ppage->ipa + (PAGE_SIZE << ppage->order))` at `mmu.c:364`, which would have printed separately. It appears in no capture — `nohuge` produces exactly two warnings, `pgtable.c:654` and `mmu.c:456` — so the error genuinely comes back from EL2 rather than from the host's own walk.

**The error code is `-1`, measured.** A kretprobe on `pkvm_call_hyp_nvhe_ppage()` — EL1 host code, so reachable — over three runs per mode (`evidence/2026-08-05-kretprobe-el2-return.txt`):

| mode | returns | values |
| --- | --- | --- |
| default | 9 | all `0` |
| `nohuge` | 21 | 18 × `0`, **3 × `4294967295`** — one per run |
| `nodirty` | 6 | all `0` |

`4294967295` is `0xFFFFFFFF`, i.e. `-1` as a 32-bit `int`. `EPERM` is 1 and no other errno is (`EINVAL` 22, `ENOENT` 2, `E2BIG` 7, `ENOMEM` 12), so the value is **`-EPERM`**. The anti-correlation therefore holds at the return-value level and not merely at the warning level: `default` produces **zero** non-zero returns.

Two method notes worth keeping. The register dump could not have given this — `w0` holds the error at the `cbz`, but the SanCov call before the `brk` clobbers `x0`, and the `#4` dump duly shows `x0 : 0`. And a first attempt at the probe recorded nothing in all three modes because `tracing_on` was `0`; the probe installs and enables without complaint and silently records nothing.

**That refutes this issue's first hypothesis.** `pkvm_call_hyp_nvhe_ppage()` (`pkvm.c:240-296`) absorbs most failures; only four returns reach the caller — three `-EINVAL` arms at `pkvm.c:263`, `:272` and `:281`, and `return err` at `:293`. An earlier revision argued that the first two — the arms that fire when EL2 answers `-E2BIG` or `-ENOENT` while `order == 0` — explained why the warning tracks page size. They do not. `-EPERM` is neither of those values, so the failure takes `pkvm.c:293` and passes the EL2 error through verbatim. **The page-size correlation therefore still wants an explanation**, and it is no longer available from the host side.

**Leading hypothesis: a page-state mismatch in the unshare check.** The unmap is an unshare at EL2 —

```
handle___pkvm_host_unmap_guest        hyp_main.rs:1094
  → __pkvm_host_unshare_guest         permissions.rs:739
    → __check_host_unshare_guest      permissions.rs:747  →  host.rs:1706
```

— and that check has exactly two `-EPERM` returns, both comparing page state:

```rust
	let state = guest_get_page_state(pte, ipa) & !PKVM_PAGE_RESTRICTED_PROT;
	if state != PKVM_PAGE_SHARED_BORROWED {
		return -(EPERM as i32);                                  /* host.rs:1722 */
	}
	...
	__host_check_page_state_range(phys_value, PAGE_SIZE << order,
				      PKVM_PAGE_SHARED_OWNED)            /* host.rs:1728 */
```

the second returning `-EPERM` from `___host_check_page_state_range()` when the *host* state disagrees. Both are exactly the state that a successful `__pkvm_host_dirty_log_guest` mutates: it runs `check_unshare()` and then re-maps the page `KVM_PGTABLE_PROT_RWX` (`permissions.rs:135-211`). When issue 003 aborts that call with `-E2BIG`, none of the mutation happens — which is the shape of the masking.

Note that `host.rs:1720` already masks `PKVM_PAGE_RESTRICTED_PROT` out before comparing, so that particular bit is tolerated; the mismatch has to be in the base state.

### The cause: the re-map writes the state away

**Confidence: full code read of both trees; not yet confirmed on hardware.** Details and quotations in `evidence/2026-08-06-upstream-provenance.txt`.

pKVM keeps a page's ownership state **inside the stage-2 PTE**, in its two software bits, and carries it around in the same word as the permissions:

```c
enum pkvm_page_state {
	PKVM_PAGE_OWNED           = 0ULL,
	PKVM_PAGE_SHARED_OWNED    = BIT(0),
	PKVM_PAGE_SHARED_BORROWED = BIT(1),
};
#define PKVM_PAGE_STATE_PROT_MASK  (KVM_PGTABLE_PROT_SW0 | KVM_PGTABLE_PROT_SW1)
```

A prot word carrying no state therefore does not mean *unchanged* — it decodes as `PKVM_PAGE_OWNED`. Which makes the two ways of writing a guest PTE behave very differently:

| | effect on the SW state bits |
| --- | --- |
| `kvm_pgtable_stage2_map(..., prot, ...)` | builds a **fresh** PTE from `prot` — state becomes whatever `prot` carries |
| `kvm_pgtable_stage2_relax_perms(..., prot, ...)` | read-modify-write of the existing leaf — **state survives** |

`relax_perms` only ever sets `S2AP_R`/`S2AP_W` and clears `XN` (`pgtable.c:1617-1640`), so it cannot disturb them. The dirty-log handler uses the other one, with a **bare** prot:

```rust
	kvm_pgtable_stage2_map(&mut vm_ref.pgt, guest_addr, PAGE_SIZE as u64,
			       host_addr,
			       KVM_PGTABLE_PROT_RWX,        /* permissions.rs:202 — no state */
			       ... stage2_mc ..., 0)
```

So a successful dirty log leaves the guest PTE valid, fully RWX, and annotated `PKVM_PAGE_OWNED` — after `check_unshare()` has just finished verifying it was `PKVM_PAGE_SHARED_BORROWED`. **The handler validates the annotation and then overwrites it.**

That the omission is an omission, and not a deliberate convention, is settled by the same crate: `guest_complete_share()` writes the identical host→guest shared mapping and does it correctly, `let prot = pkvm_mkstate(perms, PKVM_PAGE_SHARED_BORROWED);` (`guest.rs:1116`). `pkvm_mkstate()` is right there at `utils.rs:401`.

**This predicts site A specifically**, and predicts the value: `state == PKVM_PAGE_OWNED == 0`, against an expected `BIT(1)`, giving `-EPERM` — the code that was measured. It also explains why the *host*-side check (site B) is not the one that fires: the dirty-log handler only touches the guest page table, so the host's `PKVM_PAGE_SHARED_OWNED` record is still intact.

It also disposes of the `guest_ack_unshare()` lead raised in review. That fallback retries with `SHARED_BORROWED | RESTRICTED_PROT` (`guest.rs:1072-1089`), which is still not `0`, so it cannot cover this case.

**Which of the two fires is still not measured, and cannot be from EL1.** Both sites are inside the hypervisor, past the `hvc`. The kretprobe worked because `pkvm_call_hyp_nvhe_ppage()` is host code; **kprobes cannot cross the exception level**. The technique that settled issue 002 (read a register at the WARN) and the one that settled this half (probe the boundary) stop at the same wall.

**But the mechanism now offers a confirmation that *is* reachable from EL1.** If the state is genuinely zeroed, then a **second** dirty-log call on the same page must fail too — `check_unshare()` → `guest_ack_unshare()` → `__guest_check_page_state_range(..., PKVM_PAGE_SHARED_BORROWED)` would find `PKVM_PAGE_OWNED` and return `-EPERM` *before* any mapping happens. That call is reachable by write-protecting the page again (`KVM_CLEAR_DIRTY_LOG`, or a second `KVM_GET_DIRTY_LOG` round) and writing to it once more. In `nohuge` mode issue 003 is silent, so a failing `KVM_RUN` on the second round could only be this. A one-page userspace test decides it without any EL2 tooling; see Fix status.

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

There is a second, narrower consequence that the code does settle. `__pkvm_host_unshare_guest()` returns on the failed check *before* both of its effects (`permissions.rs:739-777`):

```rust
	let ret = __check_host_unshare_guest(vm, &mut phys, ipa, order);
	if ret != 0 { ...unlock...; return ret; }          /* <-- taken */

	kvm_pgtable_stage2_unmap(&mut vm_ref.pgt, ipa, PAGE_SIZE)          /* skipped */
	__host_set_page_state_range(phys, PAGE_SIZE << order, PKVM_PAGE_OWNED)  /* skipped */
```

So the page the guest wrote to under dirty logging keeps a live guest stage-2 mapping, and the **host's** record of it stays `PKVM_PAGE_SHARED_OWNED` instead of returning to `PKVM_PAGE_OWNED`, for a VM that no longer exists. Whether anything later reclaims it is exactly the `__pkvm_finalize_teardown_vm` question above.

No memory-loss measurement has been attempted for this issue. The lesson from issue 004 applies: `MemFree` at this scale is dominated by noise, so the question is better settled by following the teardown code than by weighing the machine.

## Relationship to the other issues

All four fire on the same input — an ordinary VM whose vCPU has run, then dirty logging enabled — and the mode matrix separates them experimentally:

| | 002 | 003 | 004 | **005** |
| --- | --- | --- | --- | --- |
| needs dirty logging | yes | yes | yes | yes |
| needs huge pages | no | **yes** | no | **no — the opposite** |
| needs the dirty-log call to *succeed* | no | n/a | no | **yes** |
| where | host `mmu.c:190` | EL2 `permissions.rs:147` | host `mmu.c:1749` | EL2 unmap, via `mmu.c:325` |
| provenance | upstream, **fixed** upstream | inherited `ee88afa47b55` | mixed — see 004 | inherited `ee88afa47b55` |
| migratable patch | yes (`fce886a60207`) | no — upstream deleted the feature | partly | **no** — same deletion |

**They mask each other in a chain**, which is the practical reason to file this now:

- **002 masks 003.** Without the TLB invalidation the write-protect may not be visible to the guest, so the fault that reaches the broken path is not guaranteed — which is why 003 measures 22/25 rather than 25/25 on `#4`.
- **003 masks 005.** The dirty-log hypercall aborts before EL2 mutates any state, so the unmap that would fail never gets the chance.

Fixing them in order therefore *reveals* rather than resolves: fix 002 and 003 goes to 100 %; fix 003 and **005 appears on every dirty-logging teardown**. Anyone who lands the 003 patch and then sees this warning should read it as an unmasking, not a regression.

## Upstream

Searched 2026-08-06 across `aosp/android15-6.6`, `aosp/android16-6.12`, `aosp/android17-6.18` and `torvalds/master`. Full transcript in `evidence/2026-08-06-upstream-provenance.txt`.

**This defect is inherited from ACK verbatim — klinux introduced nothing.** The EL2 handler arrives with the commit that created it:

```c
/* ACK ee88afa47b55, 2024-04-03, Vincent Donnefort
   "ANDROID: KVM: arm64: Huge page support for pKVM guest relax perm" */
int __pkvm_host_dirty_log_guest(u64 gfn, struct pkvm_hyp_vcpu *vcpu)
{
	ret = __check_host_unshare_guest(vm, &phys, ipa, 0);
	if (ret)
		goto unlock;

	ret = kvm_pgtable_stage2_map(&vm->pgt, ipa, PAGE_SIZE,
				     phys, KVM_PGTABLE_PROT_RWX,   /* <-- bare prot */
				     &vcpu->vcpu.arch.stage2_mc, 0);
```

Same `(gfn, vcpu)` signature as klinux's Rust, same hardcoded order 0, same bare prot. That commit's *host*-side hunk is klinux's `mmu.c:1749/1755/1762` line for line, `logging_active` branch and all.

**The other upstream dirty-log design has it too.** `android15-6.6` carries a different handler, `__pkvm_dirty_log(hyp_vcpu, pfn, gfn)` at `mem_protect.c:2771` — and it also finishes with `kvm_pgtable_stage2_map(..., KVM_PGTABLE_PROT_RWX, ...)`. Both designs drop the state.

**Upstream never fixed it. It deleted the feature.**

| commit | date | what it did to this line |
| --- | --- | --- |
| `ee88afa47b55` | 2024-04-03 | introduced it, bare prot |
| `6a3d47d01594` | 2025-01-28 | *"Make `__pkvm_host_dirty_log_guest()` upstream-friendly"* — reflowed the exact call, renamed the check, **kept the bare prot** |
| `6e3ff69cb190` | 2025-01-28 | *"Remove `__pkvm_host_dirty_log_guest()`"* — 43 deletions, pure removal |

The removal is justified by a redesign, not a bug: *"Now that dirty logging for no-guests is done from `user_mem_abort()` with the standard KVM logic, the hypercall is unused."* From 6.12 onward the surviving `__pkvm_host_relax_perms_guest()` does the same job through `kvm_pgtable_stage2_relax_perms()`, which cannot disturb the state bits — so 6.12, 6.18 and mainline are structurally immune rather than patched. `git log --grep` over both ACK lines for `dirty log|dirty_log|mkstate|page state` turns up no fix.

`6a3d47d01594` is worth noting for what it is: a cleanup that rewrote this call and did not see it.

## Fix status

Not fixed. **There is no patch to migrate** — the upstream remedy is a feature removal predicated on moving np-guest dirty logging into generic `user_mem_abort()`, which is a redesign of the whole np-guest memory path, not a hunk. So this one is ours to write.

**Proposed fix — restore the annotation the map destroys.** `permissions.rs:202`, in `__pkvm_host_dirty_log_guest()`:

```rust
-            KVM_PGTABLE_PROT_RWX,
+            pkvm_mkstate(KVM_PGTABLE_PROT_RWX, PKVM_PAGE_SHARED_BORROWED),
```

with `pkvm_mkstate` added to the `use crate::utils::{...}` list at `permissions.rs:28` and `PKVM_PAGE_SHARED_BORROWED` to the `consts` import.

`PKVM_PAGE_SHARED_BORROWED` is the correct value and not a guess: it is what `check_unshare()` verified the page held two statements earlier, the mapping's ownership is not meant to change (only its permissions and granularity), and it is exactly what `guest_complete_share()` writes for the same host→guest shared mapping at `guest.rs:1116`. `PKVM_PAGE_RESTRICTED_PROT` must *not* be added — it is derived from `prot != RWX`, and this mapping is full RWX.

An alternative, closer to where upstream went, is to stop using `stage2_map` here at all and reach the same end through `kvm_pgtable_stage2_relax_perms()` as 6.12 does. That is the more future-proof shape, but it cannot break a block mapping, so it only becomes available once issue 003's huge-page problem is settled. The one-line fix is orthogonal to 003 and can land first.

**Confirm before patching.** The mechanism above is a full code read of both trees, not a hardware result, and the tracker's rule is that a fix does not get built on that. Two ways to close it, cheapest first:

1. **A second dirty-logging round, from userspace.** If the state is really zeroed, a second `__pkvm_host_dirty_log_guest` on the same page must fail in `check_unshare()` with `-EPERM` *before* mapping anything. Write-protect the page again (`KVM_CLEAR_DIRTY_LOG`, or a second `KVM_GET_DIRTY_LOG` round with manual-protect) and write to it once more; in `nohuge` mode issue 003 is silent, so a failing `KVM_RUN` there can only be this. No EL2 tooling, one extension to `repro/probe-dirtylog-thp.c`.
2. **EL2 coverage capture.** Arm the `pkvm_cov` ring, run `nohuge` and `nodirty`, diff the `.rs:line` sets. Site A (`host.rs:1722`) appearing only in the failing arm confirms it directly and rules out `___host_check_page_state_range()`.

Test 1 is the one to run first; test 2 remains the definitive one and is still the first time this project has needed the ring to advance an issue rather than to measure throughput.

**Fixing this does not fix 003, and does not need 003 fixed first.** They are independent defects in the same handler: 003 is the order-0 lookup rejecting blocks, 005 is the map dropping the state. Both are inherited from `ee88afa47b55`.

## Residual hazard

Because 003 hides this, any measurement of 005's frequency taken on a `#4`-like kernel is a lower bound, and any campaign that never enables dirty logging will never see it at all. The syzkaller campaigns to date fall in that second category — the signature does not appear in the 2026-07-31 or 2026-08-05 console logs.
