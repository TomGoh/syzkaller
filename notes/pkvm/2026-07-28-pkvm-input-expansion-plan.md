# pKVM × syzkaller — input expansion to grow EL2 coverage (2026-07-28, rev 2)

**Status:** Step 0 (HVC-hit census) is **DONE** — script `scripts/pkvm/tools/hvc-census.py`, result `notes/pkvm/evidence/a1-macro-autoinject-2026-07-28/hvc-census-2026-07-28.txt` (commit `35fda2db1`). Results and their consequences for the plan are recorded in "Step 0 — RESULT" below. Boundary #1 is not yet implemented. Rev 2 is post-verification: a colleague pass caught real errors in rev 1, retracted in Context.

## Context

The macro auto-injection instrumented **all ~46 host→hyp HVCs**, so instrumentation is no longer the limiter. The live campaign **plateaued at ~7535 / 298 EL2 `.rs` lines** because the only generateable input has a 1-bit mutable surface (`hp_mode`), driving just create+map+teardown. To grow coverage we must broaden the input model so the fuzzer drives HVCs it never reaches today.

**This is rev 2 after a colleague verification pass caught real errors in rev 1 — retracted here:**
- ❌ rev 1 claimed protected secondaries are "forced STOPPED." **Wrong.** `KVM_ARM_VCPU_INIT` sets `KVM_MP_STATE_RUNNABLE` (arm.c:1456) unless `init->features[0]` has `BIT(KVM_ARM_VCPU_POWER_OFF)` (arm.c:1420-1454). rev 1's no-feature secondaries would be RUNNABLE → pvmfw branch → `pvmfw_entry_vcpu` already claimed → `-EINVAL` (hyp/nvhe/pkvm.c:571-573) → `goto destroy_vm` (pkvm.c:419-420) → **vCPU 0 never runs**. That would *regress* the campaign on ~2/3 of programs, not skip cleanly.
- ❌ rev 1 predicted multi-vCPU lights up the `last_ran`-flush branch + 2nd-context `switch.rs`. **Refuted.** `last_ran` is `memset(-1)` at init (hyp/nvhe/pkvm.c:657), so the flush branch already fires with one vCPU (it's in the 298-line baseline); and secondaries are never `KVM_RUN`, so no per-run path executes. Realistic yield is the `mp_state==STOPPED` branch (pkvm.c:568-570) + per-vCPU alloc, **single-digit lines**. (`__pkvm_init_vcpu` running 3× vs 1× adds no PCs — coverage is a set.)
- ❌ rev 1 called `__pkvm_wrprotect`/`__pkvm_dirty_log` "dead code" — they have live callers (mmu.c:1296,1307,1700); correct statement is **unreachable for *protected* VMs via host ioctls** (the `-EPERM` gate at mmu.c:2537-2540). And rev 1 overstated reuse of `pkvm_build_slotted_vm()`; the gen helper builds inline via `KVM_CREATE_VM`, reusing only `pkvm_memslot_ioctl()`/`pkvm_fw_backing()`.

**Lesson folded into the methodology:** for coverage prediction the *initial state* is the whole question — read the initializer, not just the branch (both rev-1 errors were init-value inversions: mp_state defaults RUNNABLE, `last_ran` inits to -1).

Honest reframing: the protected-VM **host-ioctl** surface yields only small, diminishing gains after create+map+teardown. So the plan **measures the real gap first**, then takes the best surviving small win, and states plainly where the big coverage actually lives (guest execution / dedicated drivers — the B frontier).

## Step 0 — HVC-hit census (do first; non-disturbing; turns selection into data)

With every HVC instrumented, the running campaign's own coverage already tells us which handlers fire. **Read-only, no campaign disturbance:** pull the corpus coverage (`curl http://127.0.0.1:56750/rawcover`), symbolize with `aarch64-linux-gnu-addr2line -e <matching vmlinux>`, get the set of covered `hyp/nvhe/rust/src/*.rs` functions, and cross-reference the HVC→handler map (the `HOST_HCALL` dispatch table `hyp_main.rs:1788-1855` + the `handle_*` handlers). Output: a **measured fired / never-fired list** over the dispatch table. Caveat to state in the result: coverage only captures owner-CPU + in-KCOV-window HVCs, i.e. exactly "what the fuzzer's inputs drive" — which is the right denominator. This replaces rev 1's code-reading candidate guesswork with a gap list, and tells us whether any *bigger* reachable win exists before we invest in multi-vCPU.

Denominator note: `HOST_HCALL` is `[Option<HcallFn>; 66]` (index 0 = `None`; 64/65 are xcore additions), so the authoritative surface is 65 live handlers, not the "~46" host `kvm_call_hyp_nvhe` call sites. Attribution must be at `handle_*` function granularity — the dispatch table itself is covered for every HVC.

## Step 0 — RESULT (executed 2026-07-28)

`vmlinux` EL2 range `[0xffff800081d89ed4,0xffff800081de7000)`; total PCs 7692, EL2 PCs 532, handler symbols resolved 63.

- **FIRED (14):** `#21 __pkvm_host_share_hyp`, `#22 __pkvm_host_unshare_hyp`, `#23 __pkvm_host_map_guest`, `#30 __kvm_vcpu_run`, `#31 __kvm_timer_set_cntvoff`, `#32/#33 __vgic_v3_{save,restore}_vmcr_aprs`, `#34 __pkvm_init_vm`, `#35 __pkvm_init_vcpu`, `#36/#37 __pkvm_{start,finalize}_teardown_vm`, `#38 __pkvm_reclaim_dying_guest_page`, `#39/#40 __pkvm_vcpu_{load,put}`.
- **NEVER FIRED (49).** By area: tracing/ftrace 9, iommu 8, tlb/vmid/cache 7, init/finalize 6, module-load 5, permissions/dirty 3, hyp-alloc-mgt 3, vgic 2, vcpu run/state 2, guest map/donate/reclaim 1, misc 1, snapshot 1, xcore 1.
- **SYMBOL NOT FOUND (2), inconclusive:** `#51 __pkvm_disable_ftrace`, `#65 xcore_unit_test_entry` (inlined/renamed).

Consequences for this plan:

1. **Boundary #1's ceiling is confirmed low.** `#35 __pkvm_init_vcpu` is **already FIRED** (pcs=2). Multi-vCPU therefore unlocks **no new handler** — only possibly new branches inside an already-covered one. This independently corroborates the single-digit-lines estimate; it must not be sold as a plateau-breaker.
2. **The largest never-fired clusters are exactly the areas rev 2 classified as frontiers** (tracing 9, iommu 8, module-load 5 = 22 of 49). The census confirms the classification rather than contradicting it — but it also quantifies how much is being deferred.
3. **`tlb/vmid/cache` (7 never-fired) deserves a look before multi-vCPU is committed to.** These are host-driven (memslot delete/move, MMU-notifier paths), so unlike tracing/iommu they may be reachable from KVM ioctls a composite can emit. `#8 __kvm_flush_vm_context` is also the boundary behind the existing VMID-rollover finding. Whether any of them is reachable *without* the `-EPERM` post-run memslot gate (mmu.c:2532-2535) is an open code question, not yet answered — treat as hypothesis until read in full.

## The three-piece kit (framework, corrected)
1. **syzlang** (`sys/linux/dev_kvm_arm64.txt`): new dims must be **bounded-mutable** (`int8[lo:hi]`/`flags`), never `const` (const is filtered from mutation, `prog/mutation.go:605-607,722-725`). Keep `fd_kvm` as a resource.
2. **executor C helper** (`executor/common_kvm_arm64.h`): full lifecycle stays in C (reuse `pkvm_memslot_ioctl()`, `pkvm_fw_backing()`; the gen helper builds the VM inline via `KVM_CREATE_VM`). Every new arg is **clamped/re-validated in C** so a stray value fails via `goto out`. Toggles must be **syscall args** (executor chroots into tmpfs; env/files don't reach it).
3. **smoke-gate test** (`sys/linux/test/arm64-*`): one per dimension value, gated before fuzzing — asserts non-hang, clean return, `LOST_IN_RING=0`, and the **specific expected new `.rs` lines** (not a coverage-total bump).

## Boundary #1 — multi-vCPU (leading surviving candidate; implement after Step 0 confirms it)

**syzlang** — one bounded dim on the gen composite (`dev_kvm_arm64.txt:127`):
```
syz_kvm_run_fw_fault_gen$arm64(fd fd_kvm, ipa_size const[0], fw_ipa const[0x7fc00000], hp_mode int8[0:1], nvcpus int8[1:3])
```

**executor C helper** (`common_kvm_arm64.h`, `syz_kvm_run_fw_fault_gen` ~:716-849): add `a4 = nvcpus`; **clamp in C mapping 0→1** (`n = a4 < 1 ? 1 : a4 > 3 ? 3 : a4`) — load-bearing: old 4-arg corpus entries load with `nvcpus = DefaultArg = 0` (`prog/encoding.go:382-388`), so 0 must mean 1, not reject. After vCPU 0's create+init (`:797-808`), before its `KVM_RUN` (`:827`), loop `i = 1..n-1`: `KVM_CREATE_VCPU(i)` → `KVM_ARM_VCPU_INIT` with **`init.features[0] |= 1u;` — `BIT(KVM_ARM_VCPU_POWER_OFF)`, bit 0** (the F1 fix — makes the secondary STOPPED so `__pkvm_init_vcpu` takes pkvm.c:568-570 instead of `-EINVAL`; arm.c:1421 strips the bit before the finalized feature copy, so it can't perturb the protected feature set — cleaner than a separate `KVM_SET_MP_STATE`). Track secondary fds; **run only vCPU 0** (unchanged); close all secondary fds in teardown (`:842-843`). vCPU 0's first run triggers lazy creation looping all vCPUs → `__pkvm_init_vcpu` ×`n`, each secondary taking the STOPPED branch.

**Footgun (must not be lost in review):** `KVM_ARM_VCPU_POWER_OFF` is defined as **0** (`arch/arm64/include/uapi/asm/kvm.h:105`), so `init.features[0] |= KVM_ARM_VCPU_POWER_OFF` is a **silent no-op** that reproduces exactly the rev-1 F1 bug. The `BIT()`/`1u` is load-bearing. The constant is also not available in the executor (nothing in `executor/` or `sys/linux/` defines it), and the surrounding code deliberately uses literals-with-comments for arm64 KVM UAPI values (`init.target = 5; // KVM_ARM_TARGET_GENERIC_V8`) — follow that convention.

**Safety:** `nvcpus∈[1:3]` + C clamp; secondaries STOPPED and never `KVM_RUN` → nothing blocks. If the POWER_OFF path ever regressed to `-EINVAL`, the failure is silent+clean — which is exactly why the smoke gate must assert `exit_reason == KVM_EXIT_MMIO` (vCPU 0 actually ran).

**Honest yield (F2/F5, corroborated by Step 0):** the `mp_state==STOPPED` branch + per-vCPU alloc — **single-digit new lines**, and **no new handler** (`__pkvm_init_vcpu` already fired). For scale: a2 dim ≈ 7 lines, a1 create boundary ≈ 54 lines. Boundary #1 is the best of the *surviving* candidates and establishes the repeatable kit; it is **not** a "break the plateau" lever and must not be sold as one.

**test** (`sys/linux/test/arm64-syz_kvm_run_fw_fault_gen_mvcpu`): `nvcpus=1` (regression), `2`, `3`, each `# requires: arch=arm64 -threaded`, e.g. `syz_kvm_run_fw_fault_gen$arm64(r0, 0x0, 0x7fc00000, 0x0, 0x2)`.

Files: `sys/linux/dev_kvm_arm64.txt`, `executor/common_kvm_arm64.h`, `sys/linux/test/arm64-*` (+ `executor/syscalls.h` regenerated). Kernel: none.

## Candidate reassessment (code-grounded)
- **relax-perms / dirty-log — INFEASIBLE** on protected VMs (dirty/readonly memslots `-EPERM`, mmu.c:2537-2540; `pkvm.enabled` set at create, pkvm.c:467). `__pkvm_relax_perms` is guest-fault-only. Drop. (Census agrees: all 3 permissions/dirty handlers never fired.)
- **multi-memslot — low host-driven value** (lazy fault; fixed pvmfw won't touch extra regions).
- **extended bounded guest run — the higher-yield next lever, medium risk.** Servicing more `KVM_RUN` exits in a *bounded* loop lets the trusted pvmfw execute further → real `switch.rs`/`sys_regs.rs`. Only pursue behind a strict smoke-gate (bounded iterations, handle each `exit_reason`, treat WFI/no-progress as done, hard cap to avoid hang).
- **tlb/vmid/cache — newly surfaced by Step 0, not yet assessed.** 7 never-fired handlers, host-driven in principle. Read the callers in full before ranking against Boundary #1.
- **PSCI / IOMMU / module / tracing / snapshot — out of scope**: host-initiated only via dedicated drivers (host SMC/CPU-hotplug, IOMMU driver, hyp-module load, tracefs), not KVM ioctls a composite can emit. Frontiers, not composite dims. (Census: 22 of the 49 never-fired handlers live here.)

## Cutover (one deliberate manager restart — the plateaued run loses nothing)
1. **Board-free build:** edit the 3 files; `make descriptions`; rebuild **manager + executor in one `make` invocation** (`GGFLAGS=-buildvcs=false make manager executor`) so git-revision strings match (the mismatch that crash-looped the manager earlier).
2. **Stop the live manager** (SIGINT); ring stays armed.
3. **Smoke-gate on N90** (ring free): first **assert `nr_pages==32`** in `/sys/kernel/debug/kvm/pkvm_cov/stats` (fresh boot comes up at the default 4 → cap 2045; the systemd ring-arm sets 32, but verify — else rule 1 fails for unrelated reasons). Then `syz-execprog` each `nvcpus` value: require non-hang, `exit_reason==KVM_EXIT_MMIO`, `LOST_IN_RING=0`, `skip_not_owner=0`, and the **specific new `.rs` lines** (STOPPED-branch / vcpu-alloc) via the `scripts/pkvm/a2` capture harness.
4. **If clean:** restart the manager on the same workdir (keeps corpus). **If not:** don't enable; diagnose (rule 3).

## Seed corpus for the batch
Export the warm corpus (`syz-db` pack of `workdir-macrocov-thru/corpus.db`) under `scripts/pkvm/` so batch machines skip cold-start; loaded via workdir / `-corpus`.

## Verification (end-to-end)
1. `make descriptions` parses; `make manager executor` clean; **manager & executor git-revisions match** (rev-check).
2. Smoke-gate (§Cutover.3): `nr_pages==32` asserted, non-hang, `exit_reason==MMIO`, `LOST_IN_RING=0`, and **the specific new lines appear** (addr2line vs matching vmlinux) — this is the pass criterion, **not** "coverage > 7535".
3. Post-restart: corpus keeps growing; no new WARN/BUG beyond the suppressed VMID one; exec/s healthy.

## Methodology rules
1. Instrument is no longer the limiter — input breadth is; confirm `LOST_IN_RING=0` before reading coverage.
2. Toggle must be a syscall arg (executor chroots).
3. Code reading is a hypothesis; hardware gives the answer — **and for coverage prediction, read the initializer not just the branch** (the two rev-1 errors were init-value inversions).
4. Smoke-gate before fuzzing: pass = the specific new lines appear + non-hang + `LOST_IN_RING=0`.
5. Measure before targeting: the Step-0 census over instrumented HVCs beats code-reading for candidate selection.

## Frontiers (documented, NOT attempted here)
- `switch.rs`/`sys_regs.rs`/context-switch — need real guest execution; protected-VM guest is the fixed pvmfw (extended-run nudges it; the full surface is the **B guest-agent** problem).
- IOMMU / module / tracing / snapshot / PSCI relay — dedicated driver/subsystem surfaces, separate fuzzers.
