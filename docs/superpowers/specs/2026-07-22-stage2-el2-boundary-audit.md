# Stage 2 — EL1→EL2 boundary + symbolization static audit (2026-07-22)

Read-only prep for Stage 2 (EL2 in-hyp KCOV). Nothing here injects KCOV into the Rust hyp; it pins down
*where* the boundary is, *which* hypercalls the fuzzed host calls reach, and *why* EL2 is currently dark.
All against `common@` (X1_RMS Rust hyp) and the deployed `6.6.30-pkvm-fuzz` vmlinux
(sha256 `c3d0c24e…`, build-id `1e36e2ee…`).

## 1. The boundary: the host→hyp hypercall interface

EL1 crosses to EL2 via `kvm_call_hyp_nvhe(<id>, …)` / `kvm_call_refill_hyp_nvhe(…)`. The EL2 side
dispatches through a **66-entry function-pointer table** `HOST_HCALL: [Option<HcallFn>; 66]`
(`hyp/nvhe/rust/src/hyp_main.rs`), id → Rust handler `handle_<id>`. 40 distinct `__pkvm_*` targets are
called from `arch/arm64/kvm/`. The ids that matter for the fuzzed lifecycle (see §2):

| id | hypercall | Rust handler | triggered by (host call) |
|----|-----------|--------------|--------------------------|
| 2  | `__pkvm_init` | `handle___pkvm_init` | boot (not fuzzed) |
| 21 | `__pkvm_host_share_hyp` | `handle___pkvm_host_share_hyp` | CREATE_VM / CREATE_VCPU (share structs) |
| 22 | `__pkvm_host_unshare_hyp` | `handle___pkvm_host_unshare_hyp` | teardown |
| 23 | `__pkvm_host_map_guest` | `handle___pkvm_host_map_guest` | **guest fault (real run) — firmware smoke** |
| 24 | `__pkvm_host_unmap_guest` | `handle___pkvm_host_unmap_guest` | guest unmap |
| 25 | `__pkvm_relax_perms` | `handle___pkvm_relax_perms` | guest fault (perm) |
| 26 | `__pkvm_wrprotect` | `handle___pkvm_wrprotect` | dirty-log wrprotect |
| 28 | `__pkvm_tlb_flush_vmid` | `handle___pkvm_tlb_flush_vmid` | mapping changes |
| 30 | `__kvm_vcpu_run` | `handle___kvm_vcpu_run` | KVM_RUN (guest entry) |
| 34 | `__pkvm_init_vm` | `handle___pkvm_init_vm` | first KVM_RUN (`pkvm_create_hyp_vm`, pkvm.c:405) |
| 35 | `__pkvm_init_vcpu` | `handle___pkvm_init_vcpu` | first KVM_RUN (per-vCPU) |
| 36 | `__pkvm_start_teardown_vm` | `handle___pkvm_start_teardown_vm` | VM close |
| 37 | `__pkvm_finalize_teardown_vm` | `handle___pkvm_finalize_teardown_vm` | VM close |
| 38 | `__pkvm_reclaim_dying_guest_page` | `handle___pkvm_reclaim_dying_guest_page` | VM close |

Ids 42–63 are tracing / iommu / module / hyp-alloc / stage2-snapshot; 64–65 are XCore stats/selftest —
**not reachable** from the current syzlang surface.

## 2. Reachability from the fuzzed host calls (what the campaign actually crosses)

- **Covered today (immediate_exit lifecycle + memslot rejects + ENABLE_CAP):** the EL1 *drivers*
  `pkvm_create_hyp_vm`/`__pkvm_create_hyp_vcpu`/`pkvm_init_host_vm` + the boundary calls #21/#34/#35 (VM +
  vCPU create/share) and #36–#38 (teardown). These are the 8 host-side `pkvm.c` functions the rawcover
  shows. The EL2 *handlers* behind them run but are invisible (§3).
- **NOT crossed today:** #23 `__pkvm_host_map_guest` and the guest-fault path — because `immediate_exit`
  returns before the guest executes. The EL1 entry `pkvm_mem_abort` (kallsyms `ffff800080087bc8`,
  kprobe-able) is **absent from the current rawcover**. The firmware reachability smoke
  (`2026-07-22-firmware-reachability-smoke-design.md`) is what first lights up #23 and `pkvm_mem_abort`.

## 3. Symbolization audit — one real blocker (corrected; verified with addr2line)

The colleague's three questions, tested against the real objects. **Correction of an earlier draft:** I
first claimed the nvhe *link* loses `.debug_line` for C — that was wrong. It came from testing symbols
(`__pkvm_init_vm`, `__pkvm_host_share_hyp`, `host_stage2_idmap`, `handle_trap`) that are actually **Rust
`#[no_mangle] pub extern "C"`** functions (pkvm.rs:4014, permissions.rs:251, host.rs:1841, hyp_main.rs:2975)
— they look like C symbols but are Rust, so their `??:?` is the *Rust* blocker, not a C link problem.

**Q3a — Producer (no instrumentation):** host (EL1) vmlinux has **5** `__sanitizer_cov_*` callbacks; the
`__kvm_nvhe_` image has **0**. So EL2 handlers run with no KCOV signal at all. Symbol space: **2942
`__kvm_nvhe_` symbols, 655 Rust-mangled** (`__kvm_nvhe__ZN…`); handlers appear as symbols, e.g.
`__kvm_nvhe__ZN9nvhe_rust8hyp_main…handle___pkvm_host_map_guest…` @ `ffff800081d9c8a0`, and `c++filt`
recovers `nvhe_rust::hyp_main::…::handle___pkvm_host_map_guest`. So **names** demangle fine.

**Q2 — the one blocker is Rust DWARF, not C, not the link:**
- **C nvhe RESOLVES all the way to final vmlinux** — `__kvm_nvhe_xcore_rust_kvm_arm_support_pmu_v3` →
  `switch.c:94`, `__kvm_nvhe___get_fault_info` → `hyp/fault.h:48`. The `__kvm_nvhe_` prefix + nvhe link +
  final link **do** preserve C `.debug_line`. (Earlier "C link loses it" is **retracted**.)
- **Rust nvhe (the real X1_RMS target) does NOT resolve at any level** — `nvhe_rust.<hash>-cgu.0:?`.
  Root cause is concrete: **`nvhe_rust.o` has no `.debug_*` sections at all** (readelf: empty), because
  `build_rust.sh` RUSTFLAGS carries **no `-C debuginfo`** (only `relocation-model/code-model/opt-level=3/
  panic=abort/force-unwind-tables=no/target-feature=-neon`) and the release profile is `debug=false`.
  ⇒ **P0 fix: build the Rust hyp with `-C debuginfo=2`** (or `CARGO_PROFILE_RELEASE_DEBUG=2`); reckon with
  `opt-level=3` inlining. Any leftover un-resolved *C* symbols after that are a separate, minor
  investigation, **not** a global link failure.

**Q1 — runtime VA → link addr (mechanism known, NOT yet end-to-end verified):** `__kvm_nvhe_` symbols sit
at kernel **link** addresses (`ffff8000…`) in vmlinux; EL2 executes at a **hyp VA** (`__hyp_va`/
`__kern_hyp_va` offset, fixed at boot, KASLR off). What is verified so far is only *nm link-address →
addr2line*; the real path *(collected EL2 hyp-VA PC → subtract offset → link addr → addr2line)* is **not**
yet demonstrated end-to-end. Status: pending, not "solved".

**Consumer gap:** `pkg/cover` (`elf.go:50-59`) does not recognize the linker-prefixed
`__kvm_nvhe___sanitizer_cov_trace_pc` callback, and `module_obj` is `.ko`-only. So Stage 2 needs a
**two-object symbolizer**: recognize the EL2 address range, apply the hyp-VA offset (Q1), strip the
`__kvm_nvhe_` prefix, and resolve against vmlinux — which works for C today and for Rust **after** the P0
debuginfo rebuild.

## 4. Attribution & harvest discipline (do NOT wrap the boundary globally)

EL2 PCs must be attributed to **the one syz call that crossed** — not harvested at every ioctl return, and
not by globally wrapping `kvm_call_hyp_nvhe()` (that folds in boot init, other threads, and non-fuzz
contexts → wrong attribution). Per-crossing rules for the reachable set:

| boundary | harvest EL2 PCs at | attribute to | risk |
|----------|--------------------|--------------|------|
| #21 share (CREATE_VM/VCPU) | return of the `kvm_share_hyp` EL1 call site | the CREATE_VM / CREATE_VCPU call | low; single-threaded ioctl |
| #34/#35 init_vm/init_vcpu | return of `pkvm_create_hyp_vm` (first `KVM_RUN`) | the vcpu-run call | the run may be `immediate_exit` — still crosses |
| #23 host_map_guest | return of the EL1 `pkvm_mem_abort`/map path | the vcpu-run call that faulted | must not leak into a *later* call |
| #30 `__kvm_vcpu_run` | around the run hypercall | the vcpu-run call | **CPU migration**: the vcpu can move CPUs; harvest on the same CPU/thread that issued the run |
| #36–38 teardown | at the fd-close destroy path | the **explicit** `close$kvmvm_protected` call | orphaning risk is the executor's **automatic** `close_fds()` (runs after per-call output, `executor.cc:1158`), NOT the explicit close |

So the harvest points are **specific EL1 return sites** (success *and* error), each fenced to the issuing
thread/CPU. Two implementation decisions to lock before coding:
- **CPU migration (#30):** choose **CPU-pin the vcpu thread** for the harvest window, *or* tag each EL2 PC
  batch with a `{cpu, generation}` and reconcile on drain. "Fenced to the issuing CPU/thread" is the
  requirement, not yet the mechanism.
- **Teardown route:** the model **already has `close$kvmvm_protected`**, so attribute teardown EL2 PCs to
  that explicit call. The un-attributable case is only the executor's automatic `close_fds()` (which fires
  after per-call output) — a program that closes explicitly avoids it.

## 5. Stage 2 prototype direction (NOT started — this is the blueprint)

**P0 prerequisite (the one real blocker, from §3): Rust hyp debuginfo.** Build the Rust hyp with
`-C debuginfo=2` (or `CARGO_PROFILE_RELEASE_DEBUG=2`) so `nvhe_rust.o` carries `.debug_*`. Do this in a
**throwaway build tree — NOT the N90 stable fuzz kernel.** Acceptance is the **full chain to the final
vmlinux**, not just the object: `addr2line` on `handle___pkvm_host_map_guest` **and** an internal
`pkvm.rs` function must yield `rust/src/*.rs:<positive line>` in the linked vmlinux (a local object having
DWARF but vmlinux not resolving is a known failure mode to rule out). (C nvhe already resolves in vmlinux.)

**Then:**
1. **Producer:** build the EL2 Rust crate with SanCov for `aarch64-unknown-none`, hyp-local
   `__sanitizer_cov_trace_pc` writing a hyp-owned ring.
2. **Delivery:** `kcov_add_pcs()` at the specific boundary return sites (§4) — **NOT** `kcov_remote_start`
   (WARN-bails; guard `kcov.c:860`) and **NOT** a global `kvm_call_hyp_nvhe()` wrap.
3. **Consumer:** two-object symbolizer (§3) — hyp-VA offset + prefix-strip + resolve against vmlinux.
4. **Validate** on the single #23 crossing (the firmware smoke path) first.

## Status (honest)
- **Rust DWARF: SOLVED** — `-C debuginfo=2` in `build_rust.sh` makes every Rust EL2 fn resolve to
  `rust/src/*.rs:<line>` in the final vmlinux (e.g. `handle___pkvm_host_map_guest → hyp_main.rs:1069`),
  verified end-to-end in a throwaway tree. Evidence: `evidence/2026-07-22-stage2-P0-rust-debuginfo.md`.
  Caveat: it's a different binary, so Stage 2 must fuzz + symbolize the **same** debuginfo build.
- **hyp-VA → vmlinux addr:** mechanism known, **not end-to-end verified** (belongs to the producer prototype).
- **EL2 PC → KCOV:** not implemented.

## Next
- Firmware reachability smoke (`2026-07-22-firmware-reachability-smoke-design.md`) — proves #23 is reached
  and lights up `pkvm_mem_abort` (host-side), independent of Stage 2; can run in parallel.
- P0 Rust-debuginfo experiment in a throwaway build tree, accepted against the final vmlinux.
